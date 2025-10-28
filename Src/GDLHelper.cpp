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

		// Геометрические записи
		struct LineRec {
			double x1, y1, x2, y2;
			int    pen;
			int    drawIndex;
		};

		struct ArcRec {
			double cx, cy, r;
			double sDeg, eDeg;
			int    pen;
			int    drawIndex;
		};

		struct CircleRec {
			double cx, cy, r;
			int    pen;
			int    drawIndex;
		};

		struct PolyRec {
			GS::Array<API_Coord> pts;
			int    pen;
			int    frameFill;   // битовая маска 1 контур /2 заливка /4 замкнуть
			int    drawIndex;
		};

		// ComplexPolyRec раньше был poly2_b{5} с дугами и penAttribute_*
		// сейчас мы упростим: будем выводить как POLY2_ замкнутый контур без дуг
		struct ComplexPolyRec {
			GS::Array<API_Coord> pts;
			int    pen;
			bool   wantFill;
			int    drawIndex;
		};

		GS::Array<LineRec>         lines;
		GS::Array<ArcRec>          arcs;
		GS::Array<CircleRec>       circles;
		GS::Array<PolyRec>         polys;
		GS::Array<ComplexPolyRec>  complexPolys;

		constexpr double PI = 3.14159265358979323846;

		auto NormDeg = [](double d) -> double {
			d = std::fmod(d, 360.0);
			if (d < 0.0) d += 360.0;
			return d;
			};

		double minX = 1e300, minY = 1e300;
		double maxX = -1e300, maxY = -1e300;

		auto UpdBBox = [&](double x, double y) {
			if (x < minX) minX = x; if (x > maxX) maxX = x;
			if (y < minY) minY = y; if (y > maxY) maxY = y;
			};

		UInt32 appended = 0;
		int drawIndex = 1;

		for (const API_Neig& n : selNeigs) {
			API_Element e = {};
			e.header.guid = n.guid;
			if (ACAPI_Element_Get(&e) != NoError)
				continue;

			switch (e.header.type.typeID) {

			case API_LineID:
			{
				const API_Coord a = e.line.begC;
				const API_Coord b = e.line.endC;
				int pen = (int)e.line.linePen.penIndex;
				if (pen < 0) pen = 0;

				lines.Push({ a.x, a.y, b.x, b.y, pen, drawIndex });
				UpdBBox(a.x, a.y);
				UpdBBox(b.x, b.y);
				++appended;
				++drawIndex;
				break;
			}

			case API_ArcID:
			{
				const API_ArcType& a = e.arc;

				double sa = a.begAng;
				double ea = a.endAng;
				double delta = ea - sa;
				const double twoPI = 2.0 * PI;
				while (delta <= -twoPI) delta += twoPI;
				while (delta > twoPI) delta -= twoPI;
				if (delta < 0.0)
					std::swap(sa, ea);

				const double sDeg = NormDeg(sa * 180.0 / PI);
				const double eDeg = NormDeg(ea * 180.0 / PI);

				int pen = (int)e.arc.linePen.penIndex;
				if (pen < 0) pen = 0;

				arcs.Push({
					a.origC.x,
					a.origC.y,
					a.r,
					sDeg,
					eDeg,
					pen,
					drawIndex
					});

				UpdBBox(a.origC.x - a.r, a.origC.y - a.r);
				UpdBBox(a.origC.x + a.r, a.origC.y + a.r);

				++appended;
				++drawIndex;
				break;
			}

			case API_CircleID:
			{
				const API_CircleType& c = e.circle;
				int pen = (int)e.circle.linePen.penIndex;
				if (pen < 0) pen = 0;

				circles.Push({ c.origC.x, c.origC.y, c.r, pen, drawIndex });

				UpdBBox(c.origC.x - c.r, c.origC.y - c.r);
				UpdBBox(c.origC.x + c.r, c.origC.y + c.r);

				++appended;
				++drawIndex;
				break;
			}

			case API_PolyLineID:
			{
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError &&
					memo.coords != nullptr)
				{
					const Int32 nSegs = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
					int pen = (int)e.polyLine.linePen.penIndex;
					if (pen < 0) pen = 0;

					// Мы НЕ будем городить poly2_b{5} для полилинии с дугами.
					// Мы просто сохраним её как ComplexPolyRec без дуг (контур).
					// В 2D потом выведем POLY2_ замкнутый.

					ComplexPolyRec cp;
					cp.pen = pen;
					cp.wantFill = false;        // полилиния без заливки
					cp.drawIndex = drawIndex;

					cp.pts.SetSize(nSegs);
					for (Int32 i = 1; i <= nSegs; ++i) {
						cp.pts[i - 1] = (*memo.coords)[i];
						UpdBBox(cp.pts[i - 1].x, cp.pts[i - 1].y);
					}

					complexPolys.Push(cp);
					++appended;
					++drawIndex;
				}
				ACAPI_DisposeElemMemoHdls(&memo);
				break;
			}

			case API_SplineID:
			{
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError &&
					memo.coords != nullptr)
				{
					const Int32 nPts = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
					int pen = (int)e.spline.linePen.penIndex;
					if (pen < 0) pen = 0;

					// как ломанные отрезки
					for (Int32 i = 1; i < nPts; ++i) {
						const API_Coord& A = (*memo.coords)[i];
						const API_Coord& B = (*memo.coords)[i + 1];

						lines.Push({ A.x, A.y, B.x, B.y, pen, drawIndex });

						UpdBBox(A.x, A.y);
						UpdBBox(B.x, B.y);

						++appended;
						++drawIndex;
					}
				}
				ACAPI_DisposeElemMemoHdls(&memo);
				break;
			}

			case API_HatchID:
			{
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError &&
					memo.coords != nullptr)
				{
					const Int32 nCoords = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord);
					if (nCoords >= 2) {
						// штриховку отдаём как замкнутый POLY2_ с заливкой
						ComplexPolyRec cp;
						cp.pen = (int)e.hatch.contPen.penIndex;
						if (cp.pen < 0) cp.pen = 0;
						cp.wantFill = true;
						cp.drawIndex = drawIndex;

						cp.pts.SetSize(nCoords - 1);
						for (Int32 i = 1; i <= nCoords - 1; ++i) {
							cp.pts[i - 1] = (*memo.coords)[i];
							UpdBBox(cp.pts[i - 1].x, cp.pts[i - 1].y);
						}

						complexPolys.Push(cp);
						++appended;
						++drawIndex;
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

		const double cx = (minX + maxX) * 0.5;
		const double cy = (minY + maxY) * 0.5;

		const double baseW = std::max(0.0, maxX - minX);
		const double baseH = std::max(0.0, maxY - minY);

		if (baseW < 1e-9 || baseH < 1e-9)
			return "Недопустимые размеры bbox.";

		auto AppendFormat = [](GS::UniString& dst, const char* fmt, ...) {
			char buf[2048];
			va_list args; va_start(args, fmt);
#if defined(_MSC_VER)
			_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
#else
			vsnprintf(buf, sizeof(buf), fmt, args);
#endif
			va_end(args);
			dst.Append(GS::UniString(buf, CC_UTF8));
			};

		// сортировки по drawIndex (пузыри оставим как было, норм)
		auto sortByDrawIndex = [](auto& arr) {
			for (UIndex i = 0; i < arr.GetSize(); ++i) {
				for (UIndex j = i + 1; j < arr.GetSize(); ++j) {
					if (arr[i].drawIndex > arr[j].drawIndex) {
						auto tmp = arr[i];
						arr[i] = arr[j];
						arr[j] = tmp;
					}
				}
			}
			};
		sortByDrawIndex(lines);
		sortByDrawIndex(arcs);
		sortByDrawIndex(circles);
		sortByDrawIndex(polys);
		sortByDrawIndex(complexPolys);

		// === финальный вывод GDL ===
		GS::UniString out;
		out.Append("! === Масштабируемый код по A,B (центр в (0,0)) ===\n");
		AppendFormat(out, "baseW = %.6f\nbaseH = %.6f\n", baseW, baseH);

		out.Append("sx = 1\n");
		out.Append("IF baseW <> 0 THEN sx = A / baseW\n");
		out.Append("sy = 1\n");
		out.Append("IF baseH <> 0 THEN sy = B / baseH\n\n");

		out.Append("MUL2 sx, sy\n\n");

		// ЛИНИИ
		for (const auto& L : lines) {
			AppendFormat(out, "drawindex %d\n", L.drawIndex);
			AppendFormat(out, "pen %d\n", L.pen);
			out.Append("line_property 0\n");
			AppendFormat(out,
				"LINE2 %.6f, %.6f, %.6f, %.6f\n\n",
				L.x1 - cx, L.y1 - cy,
				L.x2 - cx, L.y2 - cy
			);
		}

		// ДУГИ
		for (const auto& A : arcs) {
			AppendFormat(out, "drawindex %d\n", A.drawIndex);
			AppendFormat(out, "pen %d\n", A.pen);
			out.Append("line_property 0\n");
			AppendFormat(out,
				"ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n\n",
				A.cx - cx, A.cy - cy,
				A.r,
				A.sDeg,
				A.eDeg
			);
		}

		// ОКРУЖНОСТИ
		for (const auto& C : circles) {
			AppendFormat(out, "drawindex %d\n", C.drawIndex);
			AppendFormat(out, "pen %d\n", C.pen);
			out.Append("line_property 0\n");
			AppendFormat(out,
				"CIRCLE2 %.6f, %.6f, %.6f\n\n",
				C.cx - cx, C.cy - cy,
				C.r
			);
		}

		// ПРОСТЫЕ POLY2_ (если вдруг появятся; сейчас polys не заполняется,
		// но оставим на будущее)
		for (const auto& P : polys) {
			const UIndex N = P.pts.GetSize();
			if (N == 0)
				continue;

			AppendFormat(out, "drawindex %d\n", P.drawIndex);
			AppendFormat(out, "pen %d\n", P.pen);

			// нарисуем как замкнутый с заливкой если frameFill & 2
			const bool wantFill = ((P.frameFill & 2) != 0);
			if (wantFill) {
				out.Append("set fill 1\n");
			}

			// frameFill: используем 7 = контур+заливка+замкнуть (надёжно)
			out.Append("line_property 0\n");
			AppendFormat(out, "POLY2_ %u, 7,\n", (unsigned)N);

			for (UIndex i = 0; i < N; ++i) {
				const auto& c = P.pts[i];
				bool last = (i + 1 == N);
				AppendFormat(out,
					"    %.6f, %.6f, 1%s\n",
					c.x - cx, c.y - cy,
					last ? "" : ","
				);
			}
			out.Append("\n");
		}

		// COMPLEX POLYS → тоже как POLY2_ без дуг
		for (const auto& CP : complexPolys) {
			const UIndex N = CP.pts.GetSize();
			if (N == 0)
				continue;

			AppendFormat(out, "drawindex %d\n", CP.drawIndex);
			AppendFormat(out, "pen %d\n", CP.pen);
			if (CP.wantFill) {
				out.Append("set fill 1\n");
			}

			out.Append("line_property 0\n");
			// всегда закрываем контур и даём заливку если wantFill:
			// 7 = контур + заливка + замкнуть
			out.Append("POLY2_ ");
			AppendFormat(out, "%u, 7,\n", (unsigned)N);

			for (UIndex i = 0; i < N; ++i) {
				const auto& c = CP.pts[i];
				bool last = (i + 1 == N);
				AppendFormat(out,
					"    %.6f, %.6f, 1%s\n",
					c.x - cx, c.y - cy,
					last ? "" : ","
				);
			}
			out.Append("\n");
		}

		out.Append("DEL 1\n");

		return out;
	}


} // namespace GDLHelper
