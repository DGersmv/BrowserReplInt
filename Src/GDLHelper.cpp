// ============================================================================
// GDLHelper.cpp — генерация GDL: центр bbox -> (0,0) + масштаб A,B
// (берём только pen у линий; типы линий не трогаем; POLY2_ без ENDPOLY)
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
		// if (BrowserRepl::HasInstance()) BrowserRepl::GetInstance().LogToBrowser(s);
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

	// ----------------------------------------------------------------------------
	// Основной генератор
	// ----------------------------------------------------------------------------
	GS::UniString GenerateGDLFromSelection()
	{
		GS::Array<API_Neig> selNeigs;
		API_SelectionInfo selInfo = {};
		ACAPI_Selection_Get(&selInfo, &selNeigs, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty())
			return "Нет элементов для генерации.";

		// Накапливаем ВРЕМЕННО "сырые" примитивы (в абсолютных координатах);
		// окончательно выведем их уже С ВЫЧЕТОМ (cx,cy).
		struct LineRec { double x1, y1, x2, y2; int pen; };
		struct ArcRec { double cx, cy, r, sDeg, eDeg; int pen; };
		struct CircleRec { double cx, cy, r; int pen; };
		struct PolyRec { GS::Array<API_Coord> pts; int pen; int frameFill; }; // POLY2_ (только контур+заливка+закрыть)

		GS::Array<LineRec>   lines;
		GS::Array<ArcRec>    arcs;
		GS::Array<CircleRec> circles;
		GS::Array<PolyRec>   polys;

		double minX = 1e300, minY = 1e300;
		double maxX = -1e300, maxY = -1e300;

		auto UpdBBox = [&](double x, double y) {
			if (x < minX) minX = x; if (x > maxX) maxX = x;
			if (y < minY) minY = y; if (y > maxY) maxY = y;
			};

		UInt32 appended = 0;

		for (const API_Neig& n : selNeigs) {
			API_Element e = {}; e.header.guid = n.guid;
			if (ACAPI_Element_Get(&e) != NoError)
				continue;

			switch (e.header.type.typeID) {

			case API_LineID: {
				const API_Coord a = e.line.begC;
				const API_Coord b = e.line.endC;
				int pen = (int)e.line.linePen.penIndex; if (pen < 0) pen = 0;

				lines.Push({ a.x, a.y, b.x, b.y, pen });
				UpdBBox(a.x, a.y); UpdBBox(b.x, b.y);
				++appended;
				break;
			}

			case API_ArcID: {
				const API_ArcType& a = e.arc;
				double sa = a.begAng, ea = a.endAng;

				// нормализуем sweep
				double delta = ea - sa, twoPI = 2.0 * PI;
				while (delta <= -twoPI) delta += twoPI;
				while (delta > twoPI) delta -= twoPI;
				if (delta < 0.0) std::swap(sa, ea);

				const double sDeg = NormDeg(sa * 180.0 / PI);
				const double eDeg = NormDeg(ea * 180.0 / PI);

				int pen = (int)e.arc.linePen.penIndex; if (pen < 0) pen = 0;

				arcs.Push({ a.origC.x, a.origC.y, a.r, sDeg, eDeg, pen });
				UpdBBox(a.origC.x - a.r, a.origC.y - a.r);
				UpdBBox(a.origC.x + a.r, a.origC.y + a.r);
				++appended;
				break;
			}

			case API_CircleID: {
				const API_CircleType& c = e.circle;
				int pen = (int)e.circle.linePen.penIndex; if (pen < 0) pen = 0;

				circles.Push({ c.origC.x, c.origC.y, c.r, pen });
				UpdBBox(c.origC.x - c.r, c.origC.y - c.r);
				UpdBBox(c.origC.x + c.r, c.origC.y + c.r);
				++appended;
				break;
			}

			case API_PolyLineID: {
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
					const Int32 nSegs = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
					int pen = (int)e.polyLine.linePen.penIndex; if (pen < 0) pen = 0;

					for (Int32 i = 1; i <= nSegs; ++i) {
						const API_Coord& A = (*memo.coords)[i];
						const API_Coord& B = (*memo.coords)[(i < nSegs) ? i + 1 : 1];
						lines.Push({ A.x, A.y, B.x, B.y, pen });
						UpdBBox(A.x, A.y); UpdBBox(B.x, B.y);
						++appended;
					}
				}
				ACAPI_DisposeElemMemoHdls(&memo);
				break;
			}

			case API_SplineID: {
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
					const Int32 nPts = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
					int pen = (int)e.spline.linePen.penIndex; if (pen < 0) pen = 0;

					for (Int32 i = 1; i < nPts; ++i) {
						const API_Coord& A = (*memo.coords)[i];
						const API_Coord& B = (*memo.coords)[i + 1];
						lines.Push({ A.x, A.y, B.x, B.y, pen });
						UpdBBox(A.x, A.y); UpdBBox(B.x, B.y);
						++appended;
					}
				}
				ACAPI_DisposeElemMemoHdls(&memo);
				break;
			}

			case API_HatchID: {
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
					const Int32 nCoords = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord);
					if (nCoords >= 2) {
						PolyRec pr;
						pr.pen = (int)e.hatch.contPen.penIndex; if (pr.pen < 0) pr.pen = 0;
						pr.frameFill = 7; // 1=контур + 2=заливка + 4=закрыть
						pr.pts.SetSize(nCoords - 1);       // без последней дублирующей
						for (Int32 i = 1; i <= nCoords - 1; ++i) {
							pr.pts[i - 1] = (*memo.coords)[i];
							UpdBBox(pr.pts[i - 1].x, pr.pts[i - 1].y);
						}
						polys.Push(pr);
						++appended;
					}
				}
				ACAPI_DisposeElemMemoHdls(&memo);
				break;
			}

			default:
				break;
			}
		}

		if (appended == 0 || minX > maxX || minY > maxY)
			return "Нет поддерживаемых элементов.";

		// Центр bbox -> ноль
		const double cx = (minX + maxX) * 0.5;
		const double cy = (minY + maxY) * 0.5;

		// Базовые размеры bbox -> A,B
		const double baseW = std::max(0.0, maxX - minX);
		const double baseH = std::max(0.0, maxY - minY);
		if (baseW < 1e-9 || baseH < 1e-9)
			return "Недопустимые размеры bbox.";

		// --------- Формируем итоговый текст ----------
		GS::UniString out;

		

		// ===== Масштабируемый код (координаты уже вокруг (0,0); только MUL2) =====
		out.Append("! === Масштабируемый код по A,B (центр в (0,0)) ===\n");
		AppendFormat(out, "baseW = %.6f\nbaseH = %.6f\n", baseW, baseH);
		out.Append("parameters A = baseW, B = baseH\n");
		out.Append("sx = 1.0 : IF baseW <> 0 THEN sx = A / baseW\n");
		out.Append("sy = 1.0 : IF baseH <> 0 THEN sy = B / baseH\n");
		out.Append("MUL2 sx, sy\n");  // масштаб вокруг (0,0)
		for (const auto& L : lines) {
			AppendFormat(out, "pen %d\n", L.pen);
			AppendFormat(out, "LINE2 %.6f, %.6f, %.6f, %.6f\n",
				L.x1 - cx, L.y1 - cy, L.x2 - cx, L.y2 - cy);
		}
		for (const auto& A : arcs) {
			AppendFormat(out, "pen %d\n", A.pen);
			AppendFormat(out, "ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n",
				A.cx - cx, A.cy - cy, A.r, A.sDeg, A.eDeg);
		}
		for (const auto& C : circles) {
			AppendFormat(out, "pen %d\n", C.pen);
			AppendFormat(out, "CIRCLE2 %.6f, %.6f, %.6f\n",
				C.cx - cx, C.cy - cy, C.r);
		}
		for (const auto& P : polys) {
			AppendFormat(out, "! Hatch via POLY2_\n");
			AppendFormat(out, "pen %d\n", P.pen);
			out.Append("set fill 1\n");
			AppendFormat(out, "POLY2_ %d, %d,\n", (int)P.pts.GetSize(), P.frameFill);
			for (UIndex i = 0; i < P.pts.GetSize(); ++i) {
				const auto& c = P.pts[i];
				const bool needComma = (i + 1 < P.pts.GetSize());
				AppendFormat(out, "    %.6f, %.6f, 1%s\n",
					c.x - cx, c.y - cy, (needComma ? "," : ""));
			}
		}
		out.Append("DEL 1\n"); // отменяем MUL2

		Log(GS::UniString().Printf("[GDL] total=%u, bbox(%.3f..%.3f, %.3f..%.3f), center=(%.3f, %.3f)",
			appended, minX, maxX, minY, maxY, cx, cy));

		return out;
	}

} // namespace GDLHelper
