// ============================================================================
// GDLHelper.cpp — генерация обычного + масштабируемого GDL-кода из выделения
// (только pen, без типа линии)
// ============================================================================
#include "GDLHelper.hpp"
#include "BrowserRepl.hpp"
#include "ACAPinc.h"
#include "APICommon.h"

#include <cmath>
#include <cstdarg>
#include <algorithm>

namespace GDLHelper {

    static inline double NormDeg(double d)
    {
        d = std::fmod(d, 360.0);
        if (d < 0.0) d += 360.0;
        return d;
    }

    constexpr double PI = 3.14159265358979323846;

    static void Log(const GS::UniString& s)
    {
        if (BrowserRepl::HasInstance())
            BrowserRepl::GetInstance().LogToBrowser(s);
    }

    static void AppendFormat(GS::UniString& dst, const char* fmt, ...)
    {
        char buf[2048];
        va_list args; va_start(args, fmt);
#if defined(_MSC_VER)
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
        vsnprintf(buf, sizeof(buf), fmt, args);
#endif
        va_end(args);
        dst.Append(GS::UniString(buf, CC_UTF8));
    }

    // ------------------------------------------------------------------------
    // Основной генератор
    // ------------------------------------------------------------------------
    GS::UniString GenerateGDLFromSelection()
    {
        GS::Array<API_Neig> selNeigs;
        API_SelectionInfo selInfo = {};
        ACAPI_Selection_Get(&selInfo, &selNeigs, false);
        BMKillHandle((GSHandle*)&selInfo.marquee.coords);

        if (selNeigs.IsEmpty())
            return "Нет элементов для генерации.";

        GS::UniString gdlStatic;
        GS::UniString gdlScaledPrims;
        UInt32 appended = 0;

        double minX = 1e300, minY = 1e300;
        double maxX = -1e300, maxY = -1e300;
        auto UpdBBox = [&](double x, double y) {
            if (x < minX) minX = x; if (x > maxX) maxX = x;
            if (y < minY) minY = y; if (y > maxY) maxY = y;
            };

        for (const API_Neig& n : selNeigs) {
            API_Element e = {};
            e.header.guid = n.guid;
            if (ACAPI_Element_Get(&e) != NoError)
                continue;

            switch (e.header.type.typeID) {

                // ---------------- LINE2 ----------------
            case API_LineID: {
                const API_Coord a = e.line.begC;
                const API_Coord b = e.line.endC;

                // берем только penIndex
                Int32 penIndex = (Int32)e.line.linePen.penIndex;
                if (penIndex < 0) penIndex = 0;

                AppendFormat(gdlStatic, "pen %d\n", penIndex);
                AppendFormat(gdlStatic, "LINE2 %.6f, %.6f, %.6f, %.6f\n", a.x, a.y, b.x, b.y);

                AppendFormat(gdlScaledPrims, "pen %d\n", penIndex);
                AppendFormat(gdlScaledPrims, "LINE2 %.6f, %.6f, %.6f, %.6f\n", a.x, a.y, b.x, b.y);

                UpdBBox(a.x, a.y);
                UpdBBox(b.x, b.y);
                ++appended;
                break;
            }

                           // ---------------- ARC2 ----------------
            case API_ArcID: {
                const API_ArcType& arc = e.arc;
                double sa = arc.begAng, ea = arc.endAng;
                double delta = ea - sa;
                const double twoPI = 2.0 * PI;
                while (delta <= -twoPI) delta += twoPI;
                while (delta > twoPI) delta -= twoPI;
                if (delta < 0.0) std::swap(sa, ea);

                double sDeg = NormDeg(sa * 180.0 / PI);
                double eDeg = NormDeg(ea * 180.0 / PI);

                Int32 penIndex = (Int32)e.arc.linePen.penIndex;
                if (penIndex < 0) penIndex = 0;

                AppendFormat(gdlStatic, "pen %d\n", penIndex);
                AppendFormat(gdlStatic, "ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n",
                    arc.origC.x, arc.origC.y, arc.r, sDeg, eDeg);

                AppendFormat(gdlScaledPrims, "pen %d\n", penIndex);
                AppendFormat(gdlScaledPrims, "ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n",
                    arc.origC.x, arc.origC.y, arc.r, sDeg, eDeg);

                UpdBBox(arc.origC.x - arc.r, arc.origC.y - arc.r);
                UpdBBox(arc.origC.x + arc.r, arc.origC.y + arc.r);
                ++appended;
                break;
            }

                          // ---------------- CIRCLE2 ----------------
            case API_CircleID: {
                const API_CircleType& c = e.circle;

                Int32 penIndex = (Int32)e.circle.linePen.penIndex;
                if (penIndex < 0) penIndex = 0;

                AppendFormat(gdlStatic, "pen %d\n", penIndex);
                AppendFormat(gdlStatic, "CIRCLE2 %.6f, %.6f, %.6f\n", c.origC.x, c.origC.y, c.r);

                AppendFormat(gdlScaledPrims, "pen %d\n", penIndex);
                AppendFormat(gdlScaledPrims, "CIRCLE2 %.6f, %.6f, %.6f\n", c.origC.x, c.origC.y, c.r);

                UpdBBox(c.origC.x - c.r, c.origC.y - c.r);
                UpdBBox(c.origC.x + c.r, c.origC.y + c.r);
                ++appended;
                break;
            }

                             // ---------------- POLYLINE ----------------
            case API_PolyLineID: {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
                    const Int32 nSegs = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;

                    Int32 penIndex = (Int32)e.polyLine.linePen.penIndex;
                    if (penIndex < 0) penIndex = 0;

                    AppendFormat(gdlStatic, "pen %d\n", penIndex);
                    AppendFormat(gdlScaledPrims, "pen %d\n", penIndex);

                    for (Int32 i = 1; i <= nSegs; ++i) {
                        const API_Coord& A = (*memo.coords)[i];
                        const API_Coord& B = (*memo.coords)[(i < nSegs) ? i + 1 : 1];
                        AppendFormat(gdlStatic, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                        AppendFormat(gdlScaledPrims, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                        UpdBBox(A.x, A.y);
                        UpdBBox(B.x, B.y);
                        ++appended;
                    }
                }
                ACAPI_DisposeElemMemoHdls(&memo);
                break;
            }

                               // ---------------- SPLINE ----------------
            case API_SplineID: {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
                    const Int32 nPts = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;

                    Int32 penIndex = (Int32)e.spline.linePen.penIndex;
                    if (penIndex < 0) penIndex = 0;

                    AppendFormat(gdlStatic, "pen %d\n", penIndex);
                    AppendFormat(gdlScaledPrims, "pen %d\n", penIndex);

                    for (Int32 i = 1; i < nPts; ++i) {
                        const API_Coord& A = (*memo.coords)[i];
                        const API_Coord& B = (*memo.coords)[i + 1];
                        AppendFormat(gdlStatic, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                        AppendFormat(gdlScaledPrims, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                        UpdBBox(A.x, A.y);
                        UpdBBox(B.x, B.y);
                        ++appended;
                    }
                }
                ACAPI_DisposeElemMemoHdls(&memo);
                break;
            }

                             // ---------------- HATCH ----------------
            case API_HatchID: {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
                    const API_HatchType& h = e.hatch;

                    const Int32 nCoords = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord);     // с замыкающей
                    const Int32 nSubPolys = (memo.pends != nullptr) ? (BMGetHandleSize((GSHandle)memo.pends) / sizeof(Int32) - 1) : 1;
                    const Int32 nArcs = (memo.parcs != nullptr) ? (BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc)) : 0;

                    auto findArc = [&](Int32 begIdx) -> const API_PolyArc* {
                        for (Int32 k = 0; k < nArcs; ++k) {
                            if ((*memo.parcs)[k].begIndex == begIdx) return &(*memo.parcs)[k];
                        }
                        return nullptr;
                        };

                    // только контурное перо (без типа линии)
                    Int32 contPen = (Int32)h.contPen.penIndex; if (contPen < 0) contPen = 0;

                    // будем накапливать тело полигона и считать узлы
                    GS::UniString bodyStatic, bodyScaled;
                    Int32 nodesStatic = 0, nodesScaled = 0;

                    auto processOne = [&](GS::UniString& body, Int32& nodeCounter)
                        {
                            auto emit = [&](double x, double y, Int32 s) {
                                AppendFormat(body, "    %.6f, %.6f, %d,\n", x, y, s);
                                ++nodeCounter;
                                UpdBBox(x, y);
                                };

                            for (Int32 sp = 0; sp < nSubPolys; ++sp) {
                                const Int32 start = (memo.pends ? (*memo.pends)[sp] : 0) + 1;
                                const Int32 end = (memo.pends ? (*memo.pends)[sp + 1] : nCoords - 1); // последний индекс вершины этого контура

                                // стартовая точка контура
                                const API_Coord& S = (*memo.coords)[start];
                                emit(S.x, S.y, 600);                  // задали старт (код 600)

                                for (Int32 i = start; i <= end; ++i) {
                                    const Int32 iNext = (i < end) ? (i + 1) : start;
                                    const API_Coord& A = (*memo.coords)[i];
                                    const API_Coord& B = (*memo.coords)[iNext];

                                    if (const API_PolyArc* arcRec = findArc(i)) {
                                        // восстанавливаем центр/радиус
                                        const double ang = arcRec->arcAngle;             // рад (знак = направление: +CCW, -CW)
                                        const double dx = B.x - A.x, dy = B.y - A.y;
                                        const double chord = std::sqrt(dx * dx + dy * dy);

                                        if (chord < 1e-9 || std::fabs(ang) < 1e-9) {
                                            emit(B.x, B.y, 1);
                                            continue;
                                        }

                                        const double r = std::fabs(chord / (2.0 * std::sin(std::fabs(ang) * 0.5)));
                                        const double mx = (A.x + B.x) * 0.5;
                                        const double my = (A.y + B.y) * 0.5;
                                        const double len = chord;
                                        const double nx = -dy / len, ny = dx / len;      // нормаль влево от A->B
                                        const double d = std::sqrt(std::max(r * r - 0.25 * chord * chord, 0.0));
                                        const double cx = mx + (ang > 0.0 ? nx : -nx) * d;
                                        const double cy = my + (ang > 0.0 ? ny : -ny) * d;

                                        // статус-коды POLY2_: центр -> дуга по углу
                                        emit(cx, cy, 900);                                // задали центр для следующей дуги
                                        const double aDeg = ang * 180.0 / PI;             // можно с знаком (CW/CCW)
                                        emit(0.0, aDeg, 4000 + 1);                        // дуга по центру и углу (видимая)
                                        // конец дуги попадёт в B автоматически
                                    }
                                    else {
                                        emit(B.x, B.y, 1);                                // обычный сегмент
                                    }
                                }

                                // разделитель контура (для дыр), кроме последнего
                                if (sp < nSubPolys - 1) emit(0.0, 0.0, -1);
                            }
                        };

                    processOne(bodyStatic, nodesStatic);
                    processOne(bodyScaled, nodesScaled);

                    const Int32 frameFill = 1 + 2 + 4;   // контур + заливка + замкнуть
                    AppendFormat(gdlStatic, "! Hatch via POLY2_ (дуги кодами 900/3000/4000)\n");
                    AppendFormat(gdlStatic, "pen %d\n", contPen);
                    AppendFormat(gdlStatic, "set fill %d\n", 1);
                    AppendFormat(gdlStatic, "POLY2_ %d, %d,\n", nodesStatic, frameFill);
                    gdlStatic.Append(bodyStatic);
                    gdlStatic.Append("ENDPOLY\n");

                    AppendFormat(gdlScaledPrims, "! Hatch via POLY2_ (дуги кодами 900/3000/4000)\n");
                    AppendFormat(gdlScaledPrims, "pen %d\n", contPen);
                    AppendFormat(gdlScaledPrims, "set fill %d\n", 1);
                    AppendFormat(gdlScaledPrims, "POLY2_ %d, %d,\n", nodesScaled, frameFill);
                    gdlScaledPrims.Append(bodyScaled);
                    gdlScaledPrims.Append("ENDPOLY\n");

                    ++appended;
                }
                ACAPI_DisposeElemMemoHdls(&memo);
                break;
            }




            default:
                break;
            }
        }

        if (appended == 0 || minX > maxX || minY > maxY) {
            return "Нет поддерживаемых элементов.";
        }

        double baseW = std::max(0.0, maxX - minX);
        double baseH = std::max(0.0, maxY - minY);
        if (baseW < 1e-9 || baseH < 1e-9)
            return "Недопустимые размеры.";

        GS::UniString out;
        out.Append("! === Обычный код ===\n");
        out.Append(gdlStatic);
        out.Append("\n");
        out.Append("! === Масштабируемый код по A,B ===\n");
        AppendFormat(out, "baseW = %.6f\nbaseH = %.6f\n", baseW, baseH);
        AppendFormat(out, "ox = %.6f\noy = %.6f\n", minX, minY);
        out.Append("parameters A = baseW, B = baseH\n");
        out.Append("sx = A / baseW\nsy = B / baseH\n");
        out.Append("ADD2 -ox, -oy\n");
        out.Append("MUL2 sx, sy\n");
        out.Append(gdlScaledPrims);
        out.Append("DEL 2\n");

        Log(GS::UniString().Printf("[GDL] total=%u bbox=(%.3f..%.3f, %.3f..%.3f)", appended, minX, maxX, minY, maxY));
        return out;
    }

} // namespace GDLHelper
