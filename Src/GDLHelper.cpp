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
		struct LineRec { double x1, y1, x2, y2; int pen; int lineType; int drawIndex; };
		struct ArcRec { double cx, cy, r, sDeg, eDeg; int pen; int drawIndex; };
		struct CircleRec { double cx, cy, r; int pen; int drawIndex; };
		struct PolyRec { GS::Array<API_Coord> pts; int pen; int frameFill; int drawIndex; }; // POLY2_ (только контур+заливка+закрыть)
		struct ComplexPolyRec { 
			GS::Array<API_Coord> pts; 
			GS::Array<API_PolyArc> arcs; 
			int pen; 
			int frameFill; 
			int drawIndex; 
		}; // poly2_b{5} с дугами

		GS::Array<LineRec>   lines;
		GS::Array<ArcRec>    arcs;
		GS::Array<CircleRec> circles;
		GS::Array<PolyRec>   polys;
		GS::Array<ComplexPolyRec> complexPolys;

		double minX = 1e300, minY = 1e300;
		double maxX = -1e300, maxY = -1e300;

		auto UpdBBox = [&](double x, double y) {
			if (x < minX) minX = x; if (x > maxX) maxX = x;
			if (y < minY) minY = y; if (y > maxY) maxY = y;
			};

		UInt32 appended = 0;
		int drawIndex = 1; // Начинаем с 1

		for (const API_Neig& n : selNeigs) {
			API_Element e = {}; e.header.guid = n.guid;
			if (ACAPI_Element_Get(&e) != NoError)
				continue;

			switch (e.header.type.typeID) {

			case API_LineID: {
				const API_Coord a = e.line.begC;
				const API_Coord b = e.line.endC;
				int pen = (int)e.line.linePen.penIndex; if (pen < 0) pen = 0;
				int lineType = 0; // lineType не доступен в API_ExtendedPenType

				lines.Push({ a.x, a.y, b.x, b.y, pen, lineType, drawIndex });
				UpdBBox(a.x, a.y); UpdBBox(b.x, b.y);
				++appended;
				++drawIndex;
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

				arcs.Push({ a.origC.x, a.origC.y, a.r, sDeg, eDeg, pen, drawIndex });
				UpdBBox(a.origC.x - a.r, a.origC.y - a.r);
				UpdBBox(a.origC.x + a.r, a.origC.y + a.r);
				++appended;
				++drawIndex;
				break;
			}

			case API_CircleID: {
				const API_CircleType& c = e.circle;
				int pen = (int)e.circle.linePen.penIndex; if (pen < 0) pen = 0;

				circles.Push({ c.origC.x, c.origC.y, c.r, pen, drawIndex });
				UpdBBox(c.origC.x - c.r, c.origC.y - c.r);
				UpdBBox(c.origC.x + c.r, c.origC.y + c.r);
				++appended;
				++drawIndex;
				break;
			}

			case API_PolyLineID: {
				API_ElementMemo memo = {};
				if (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
					const Int32 nSegs = BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord) - 1;
					int pen = (int)e.polyLine.linePen.penIndex; if (pen < 0) pen = 0;
					int lineType = 0; // lineType не доступен в API_ExtendedPenType

					// Проверяем, есть ли дуги
					bool hasArcs = (memo.parcs != nullptr && BMGetHandleSize((GSHandle)memo.parcs) > 0);
					
					if (hasArcs) {
						// Создаем сложный полигон с дугами
						ComplexPolyRec cpoly;
						cpoly.pen = pen;
						cpoly.frameFill = 0; // только контур
						cpoly.drawIndex = drawIndex;
						
						// Копируем координаты
						cpoly.pts.SetSize(nSegs);
						for (Int32 i = 1; i <= nSegs; ++i) {
							cpoly.pts[i - 1] = (*memo.coords)[i];
							UpdBBox(cpoly.pts[i - 1].x, cpoly.pts[i - 1].y);
						}
						
						// Копируем дуги
						const Int32 nArcs = (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc));
						cpoly.arcs.SetSize(nArcs);
						for (Int32 i = 0; i < nArcs; ++i) {
							cpoly.arcs[i] = (*memo.parcs)[i];
						}
						
						complexPolys.Push(cpoly);
						++appended;
						++drawIndex;
					} else {
						// Обычные линии
						for (Int32 i = 1; i <= nSegs; ++i) {
							const API_Coord& A = (*memo.coords)[i];
							const API_Coord& B = (*memo.coords)[(i < nSegs) ? i + 1 : 1];
							lines.Push({ A.x, A.y, B.x, B.y, pen, lineType, drawIndex });
							UpdBBox(A.x, A.y); UpdBBox(B.x, B.y);
							++appended;
							++drawIndex;
						}
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
					int lineType = 0; // lineType не доступен в API_ExtendedPenType

					for (Int32 i = 1; i < nPts; ++i) {
						const API_Coord& A = (*memo.coords)[i];
						const API_Coord& B = (*memo.coords)[i + 1];
						lines.Push({ A.x, A.y, B.x, B.y, pen, lineType, drawIndex });
						UpdBBox(A.x, A.y); UpdBBox(B.x, B.y);
						++appended;
						++drawIndex;
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
						// Проверяем, есть ли дуги в штриховке
						bool hasArcs = (memo.parcs != nullptr && BMGetHandleSize((GSHandle)memo.parcs) > 0);
						
					// Всегда создаем сложный полигон для штриховок (poly2_b{5})
					ComplexPolyRec cpoly;
					cpoly.pen = (int)e.hatch.contPen.penIndex; if (cpoly.pen < 0) cpoly.pen = 0;
					cpoly.frameFill = 7; // 1=контур + 2=заливка + 4=закрыть
					cpoly.drawIndex = drawIndex;
					
					// Копируем координаты
					cpoly.pts.SetSize(nCoords - 1);
					for (Int32 i = 1; i <= nCoords - 1; ++i) {
						cpoly.pts[i - 1] = (*memo.coords)[i];
						UpdBBox(cpoly.pts[i - 1].x, cpoly.pts[i - 1].y);
					}
					
					// Копируем дуги (если есть)
					if (hasArcs) {
						const Int32 nArcs = (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc));
						cpoly.arcs.SetSize(nArcs);
						for (Int32 i = 0; i < nArcs; ++i) {
							cpoly.arcs[i] = (*memo.parcs)[i];
						}
					} else {
						// Создаем пустой массив дуг
						cpoly.arcs.SetSize(0);
					}
					
					complexPolys.Push(cpoly);
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
		
		// Сортируем все элементы по drawIndex (простая пузырьковая сортировка)
		for (UIndex i = 0; i < lines.GetSize(); ++i) {
			for (UIndex j = i + 1; j < lines.GetSize(); ++j) {
				if (lines[i].drawIndex > lines[j].drawIndex) {
					LineRec temp = lines[i];
					lines[i] = lines[j];
					lines[j] = temp;
				}
			}
		}
		for (UIndex i = 0; i < arcs.GetSize(); ++i) {
			for (UIndex j = i + 1; j < arcs.GetSize(); ++j) {
				if (arcs[i].drawIndex > arcs[j].drawIndex) {
					ArcRec temp = arcs[i];
					arcs[i] = arcs[j];
					arcs[j] = temp;
				}
			}
		}
		for (UIndex i = 0; i < circles.GetSize(); ++i) {
			for (UIndex j = i + 1; j < circles.GetSize(); ++j) {
				if (circles[i].drawIndex > circles[j].drawIndex) {
					CircleRec temp = circles[i];
					circles[i] = circles[j];
					circles[j] = temp;
				}
			}
		}
		for (UIndex i = 0; i < polys.GetSize(); ++i) {
			for (UIndex j = i + 1; j < polys.GetSize(); ++j) {
				if (polys[i].drawIndex > polys[j].drawIndex) {
					PolyRec temp = polys[i];
					polys[i] = polys[j];
					polys[j] = temp;
				}
			}
		}
		for (UIndex i = 0; i < complexPolys.GetSize(); ++i) {
			for (UIndex j = i + 1; j < complexPolys.GetSize(); ++j) {
				if (complexPolys[i].drawIndex > complexPolys[j].drawIndex) {
					ComplexPolyRec temp = complexPolys[i];
					complexPolys[i] = complexPolys[j];
					complexPolys[j] = temp;
				}
			}
		}
		
		for (const auto& L : lines) {
			AppendFormat(out, "drawindex %d\n", L.drawIndex);
			AppendFormat(out, "pen %d\n", L.pen);
			if (L.lineType > 0) {
				AppendFormat(out, "set line_type %d\n", L.lineType);
			}
			AppendFormat(out, "line_property %d\n", L.lineType);
			AppendFormat(out, "LINE2 %.6f, %.6f, %.6f, %.6f\n",
				L.x1 - cx, L.y1 - cy, L.x2 - cx, L.y2 - cy);
		}
		for (const auto& A : arcs) {
			AppendFormat(out, "drawindex %d\n", A.drawIndex);
			AppendFormat(out, "pen %d\n", A.pen);
			AppendFormat(out, "ARC2 %.6f, %.6f, %.6f, %.6f, %.6f\n",
				A.cx - cx, A.cy - cy, A.r, A.sDeg, A.eDeg);
		}
		for (const auto& C : circles) {
			AppendFormat(out, "drawindex %d\n", C.drawIndex);
			AppendFormat(out, "pen %d\n", C.pen);
			AppendFormat(out, "CIRCLE2 %.6f, %.6f, %.6f\n",
				C.cx - cx, C.cy - cy, C.r);
		}
		for (const auto& P : polys) {
			AppendFormat(out, "drawindex %d\n", P.drawIndex);
			AppendFormat(out, "! Hatch via POLY2_\n");
			AppendFormat(out, "pen %d\n", P.pen);
			out.Append("set fill 1\n");
			AppendFormat(out, "POLY2_ %d, %d,\n", (int)P.pts.GetSize(), P.frameFill); // используем frameFill
			for (UIndex i = 0; i < P.pts.GetSize(); ++i) {
				const auto& c = P.pts[i];
				const bool needComma = (i + 1 < P.pts.GetSize());
				AppendFormat(out, "    %.6f, %.6f, 1%s\n",
					c.x - cx, c.y - cy, (needComma ? "," : ""));
			}
		}
		
		// Генерируем сложные полигоны с дугами (poly2_b{5})
		for (const auto& CP : complexPolys) {
			AppendFormat(out, "drawindex %d\n", CP.drawIndex);
			AppendFormat(out, "! Complex polygon with arcs via poly2_b{5}\n");
			AppendFormat(out, "pen %d\n", CP.pen);
			if (CP.frameFill > 0) {
				out.Append("set fill 1\n");
			}
			
			// Генерируем poly2_b{5} с дугами
			AppendFormat(out, "poly2_b{5} %d, %d, %d, %d, %d, %d,\n", 
				(int)CP.pts.GetSize(), CP.frameFill, 1, 3, CP.pen, CP.pen);
			
			// Генерируем координаты (только x, y)
			for (UIndex i = 0; i < CP.pts.GetSize(); ++i) {
				const auto& c = CP.pts[i];
				AppendFormat(out, "    %.6f, %.6f, %d,\n",
					c.x - cx, c.y - cy, 33); // код точки
			}
			
			// Генерируем дуги (если есть)
			if (CP.arcs.GetSize() > 0) {
				for (UIndex i = 0; i < CP.arcs.GetSize(); ++i) {
					const auto& arc = CP.arcs[i];
					AppendFormat(out, "    %.6f, %.6f, %d,\n",
						CP.pts[arc.begIndex - 1].x - cx, 
						CP.pts[arc.begIndex - 1].y - cy, 
						900); // код дуги
					AppendFormat(out, "    0, %.6f, %d,\n",
						arc.arcAngle * 180.0 / PI, 4033); // угол в градусах
				}
			}
		}
		out.Append("DEL 1\n"); // отменяем MUL2

		Log(GS::UniString().Printf("[GDL] total=%u, bbox(%.3f..%.3f, %.3f..%.3f), center=(%.3f, %.3f)",
			appended, minX, maxX, minY, maxY, cx, cy));

		return out;
	}

} // namespace GDLHelper
