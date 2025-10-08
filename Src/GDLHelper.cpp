#include "GDLHelper.hpp"
#include "BrowserRepl.hpp"
#include "ACAPinc.h"
#include "APICommon.h"
#include <cmath>
#include <cstdarg>
#include <algorithm> // для std::swap

namespace GDLHelper {

    static inline double NormDeg(double d)
    {
        d = std::fmod(d, 360.0);
        if (d < 0.0) d += 360.0;
        return d;
    }

    constexpr double PI = 3.14159265358979323846;

    static void Log(const GS::UniString& s) {
        if (BrowserRepl::HasInstance()) BrowserRepl::GetInstance().LogToBrowser(s);
    }

    // безопасный printf -> UniString
    static void AppendFormat(GS::UniString& dst, const char* fmt, ...) {
        char buf[1024];
        va_list args; va_start(args, fmt);
#if defined(_MSC_VER)
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
        vsnprintf(buf, sizeof(buf), fmt, args);
#endif
        va_end(args);
        dst.Append(GS::UniString(buf, CC_UTF8));
    }

    GS::UniString GenerateGDLFromSelection()
    {
        GS::Array<API_Neig> selNeigs;
        API_SelectionInfo selInfo = {};
        ACAPI_Selection_Get(&selInfo, &selNeigs, false);
        BMKillHandle((GSHandle*)&selInfo.marquee.coords);

        GS::UniString gdl;
        UInt32 appended = 0;

        for (const API_Neig& n : selNeigs) {
            API_Element e = {};
            e.header.guid = n.guid;
            if (ACAPI_Element_Get(&e) != NoError)
                continue;

            switch (e.header.type.typeID) {

            case API_LineID: {
                const API_Coord a = e.line.begC;
                const API_Coord b = e.line.endC;
                AppendFormat(gdl, "LINE2 %.6f, %.6f, %.6f, %.6f\n", a.x, a.y, b.x, b.y);
                ++appended; Log("[GDL] +LINE2");
                break;
            }

            case API_ArcID: {
                const API_ArcType& a = e.arc;

                // углы в радианах
                double sa = a.begAng;
                double ea = a.endAng;

                // нормализуем разность в (-2π, 2π)
                double delta = ea - sa;
                const double twoPI = 2.0 * PI;
                while (delta <= -twoPI) delta += twoPI;
                while (delta > twoPI) delta -= twoPI;

                // если дуга задана по часовой (delta < 0) — меняем местами start/end
                if (delta < 0.0) std::swap(sa, ea);

                // в градусы + нормализация [0..360)
                const double sDeg = NormDeg(sa * 180.0 / PI);
                const double eDeg = NormDeg(ea * 180.0 / PI);

                AppendFormat(gdl, "ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n",
                    a.origC.x, a.origC.y, a.r, sDeg, eDeg);

                ++appended; Log("[GDL] +ARC2 (fixed CW/CCW)");
                break;
            }


            case API_CircleID: {
                const API_CircleType& c = e.circle;
                AppendFormat(gdl, "CIRCLE2 %.6f, %.6f, %.6f\n", c.origC.x, c.origC.y, c.r);
                ++appended; Log("[GDL] +CIRCLE2");
                break;
            }

            case API_PolyLineID: {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
                    const Int32 nSegs = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
                    const Int32 nArcs = (memo.parcs != nullptr) ? BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc) : 0;

                    auto normDeg = [](double d)->double {
                        d = fmod(d, 360.0);
                        if (d < 0.0) d += 360.0;
                        return d;
                        };

                    for (Int32 i = 1; i <= nSegs; ++i) {
                        const API_Coord& A = (*memo.coords)[i];
                        const API_Coord& B = (*memo.coords)[(i < nSegs) ? i + 1 : 1];

                        // есть ли дуга, начинающаяся с этого ребра?
                        const API_PolyArc* arcRec = nullptr;
                        for (Int32 k = 0; k < nArcs; ++k) {
                            if ((*memo.parcs)[k].begIndex == i) { arcRec = &(*memo.parcs)[k]; break; }
                        }

                        if (arcRec == nullptr) {
                            AppendFormat(gdl, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                            ++appended;
                            continue;
                        }

                        // восстановление центра и радиуса по хорде и дуге
                        const double ang = arcRec->arcAngle;           // рад, знак = направление
                        const double dx = B.x - A.x, dy = B.y - A.y;
                        const double chord = std::sqrt(dx * dx + dy * dy);
                        if (chord < 1e-9 || std::fabs(ang) < 1e-9) {
                            AppendFormat(gdl, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                            ++appended;
                            continue;
                        }

                        const double r = std::fabs(chord / (2.0 * std::sin(std::fabs(ang) / 2.0)));
                        const double mx = (A.x + B.x) * 0.5, my = (A.y + B.y) * 0.5;
                        const double len = chord;
                        const double nx = -dy / len, ny = dx / len;                       // нормаль "влево" от A->B
                        const double d = std::sqrt(std::max(r * r - (chord * 0.5) * (chord * 0.5), 0.0));
                        const double cx = mx + (ang > 0.0 ? nx : -nx) * d;                 // центр по знаку дуги
                        const double cy = my + (ang > 0.0 ? ny : -ny) * d;

                        double aA = std::atan2(A.y - cy, A.x - cx) * 180.0 / PI;           // углы в градусах
                        double aB = std::atan2(B.y - cy, B.x - cx) * 180.0 / PI;
                        aA = normDeg(aA);
                        aB = normDeg(aB);

                        // ВАЖНО: если дуга задана по часовой (ang < 0), меняем местами start/end
                        double startDeg = (ang >= 0.0) ? aA : aB;
                        double endDeg = (ang >= 0.0) ? aB : aA;

                        AppendFormat(gdl, "ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n", cx, cy, r, startDeg, endDeg);
                        ++appended;
                    }
                }
                ACAPI_DisposeElemMemoHdls(&memo);
                Log("[GDL] +Polyline fixed (CW/CCW)");
                break;
            }

            case API_SplineID: {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
                    const Int32 nPts = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
                    for (Int32 i = 1; i < nPts; ++i) {
                        const API_Coord& A = (*memo.coords)[i];
                        const API_Coord& B = (*memo.coords)[i + 1];
                        AppendFormat(gdl, "LINE2 %.6f, %.6f, %.6f, %.6f\n", A.x, A.y, B.x, B.y);
                        ++appended;
                    }
                }
                ACAPI_DisposeElemMemoHdls(&memo);
                Log("[GDL] +Spline->lines");
                break;
            }

            case API_HatchID: {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
                    const Int32 nCoords = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord);
                    const Int32 nMain = nCoords - 1;
                    AppendFormat(gdl, "POLY2_B 1, 0, 0, 0, 0, 0, 0, 0, %d,\n", nMain);
                    for (Int32 i = 1; i <= nMain; ++i) {
                        const API_Coord& C = (*memo.coords)[i];
                        AppendFormat(gdl, "    %.6f, %.6f,\n", C.x, C.y);
                    }
                    AppendFormat(gdl, "ENDPOLY\n\n");
                }
                ACAPI_DisposeElemMemoHdls(&memo);
                Log("[GDL] +Hatch->POLY2_B");
                break;
            }

            default:
                break;
            }
        }

        Log(GS::UniString().Printf("[GDL] total appended = %u, length = %u", appended, (unsigned)gdl.GetLength()));

        if (appended == 0 || gdl.IsEmpty())
            gdl = "Нет поддерживаемых элементов (линий, дуг, окружностей, сплайнов, штриховок).";

        return gdl;
    }

} // namespace GDLHelper
