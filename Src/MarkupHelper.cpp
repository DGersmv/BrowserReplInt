// ============================================================================
// MarkupHelper.cpp - автоматическая разметка элементов размерами
// ============================================================================

#include "MarkupHelper.hpp"
#include "BrowserRepl.hpp"

#include "ACAPinc.h"
#include "APICommon.h"
#include "APIdefs_Elements.h"

#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>

namespace MarkupHelper {

	// ============================================================================
	// Globals
	// ============================================================================
	static double g_stepMeters = 1.0; // Шаг по линии направления (м, внутр. ед.)

	// ============================================================================
	// Logginggg
	// ============================================================================
	static inline void Log(const GS::UniString& msg)
	{
		if (BrowserRepl::HasInstance())
			BrowserRepl::GetInstance().LogToBrowser("[Markup] " + msg);
		ACAPI_WriteReport("[Markup] %s", false, msg.ToCStr().Get());
	}

	// ============================================================================
	// Math helpers
	// ============================================================================
	struct Vec2 {
		double x, y;
		Vec2() : x(0), y(0) {}
		Vec2(double x_, double y_) : x(x_), y(y_) {}
		Vec2(const API_Coord& c) : x(c.x), y(c.y) {}

		Vec2 operator- (const Vec2& v) const { return Vec2(x - v.x, y - v.y); }
		Vec2 operator+ (const Vec2& v) const { return Vec2(x + v.x, y + v.y); }
		Vec2 operator* (double s)     const { return Vec2(x * s, y * s); }
		double dot(const Vec2& v)  const { return x * v.x + y * v.y; }
		double cross(const Vec2& v)  const { return x * v.y - y * v.x; }
		double length()              const { return std::sqrt(x * x + y * y); }
		Vec2 normalized()            const { double L = length(); return (L > 1e-12) ? Vec2(x / L, y / L) : Vec2(0, 0); }
		Vec2 perpendicular()         const { return Vec2(-y, x); } // +90°
		API_Coord toCoord()          const { API_Coord c; c.x = x; c.y = y; return c; }
	};

	static double PolygonArea(const std::vector<Vec2>& poly)
	{
		if (poly.size() < 3) return 0.0;
		double a = 0.0;
		for (size_t i = 0, n = poly.size(); i < n; ++i) {
			const Vec2& p = poly[i];
			const Vec2& q = poly[(i + 1) % n];
			a += p.x * q.y - q.x * p.y;
		}
		return std::abs(a) * 0.5;
	}

	// ============================================================================
	// Пересечение луча с отрезком
	// ============================================================================
	static bool RaySegmentIntersection(const Vec2& origin, const Vec2& dirUnit,
		const Vec2& segA, const Vec2& segB,
		double& outT, double& outDist)
	{
		const Vec2 v = segB - segA;
		const Vec2 w = origin - segA;

		const double denom = dirUnit.cross(v);
		if (std::fabs(denom) < 1e-12) return false; // параллельны

		const double s = dirUnit.cross(w) / denom; // параметр отрезка [0..1]
		const double rayT = v.cross(w) / denom;  // параметр луча [0..+inf)

		if (s < 0.0 || s > 1.0) return false;
		if (rayT < -1e-12)      return false;

		outT = rayT;
		outDist = std::fabs(rayT); // dirUnit — единичный
		return true;
	}

	// ============================================================================
	// Аппроксимация дуги (по API_PolyArc)
	// ============================================================================
	static int FindArcRecord(const API_PolyArc* parcs, Int32 nArcs, Int32 begIndex)
	{
		if (parcs == nullptr || nArcs <= 0) return -1;
		for (Int32 i = 0; i < nArcs; ++i)
			if (parcs[i].begIndex == begIndex)
				return (int)i;
		return -1;
	}

	static void AppendArcApprox(const API_Coord& a, const API_Coord& b, double arcAngle,
		std::vector<Vec2>& out, double segLen = 0.10) // Уменьшил segLen для точности
	{
		Vec2 P0(a), P1(b);
		Vec2 chord = P1 - P0;
		const double L = chord.length();
		if (L < 1e-9 || std::fabs(arcAngle) < 1e-6) { out.emplace_back(P1); return; }

		// Более точная формула для радиуса дуги
		const double half = L * 0.5;
		const double sinHalf = std::sin(std::fabs(arcAngle) * 0.5);
		if (sinHalf < 1e-9) { out.emplace_back(P1); return; }
		
		const double R = half / sinHalf;
		const double h = std::sqrt(std::max(R * R - half * half, 0.0));

		Vec2 dir = chord.normalized();
		Vec2 left = dir.perpendicular();
		int  sgn = (arcAngle > 0.0) ? 1 : -1;
		Vec2 C = (P0 + P1) * 0.5 + left * (double)(-sgn) * h;

		double a0 = std::atan2(P0.y - C.y, P0.x - C.x);
		// Адаптивное количество сегментов в зависимости от угла
		int N = std::max(8, (int)std::ceil((std::fabs(arcAngle) * R) / segLen));
		N = std::min(N, 64); // Ограничиваем максимум
		
		for (int i = 1; i <= N; ++i) {
			double ang = a0 + (arcAngle * i) / (double)N;
			out.emplace_back(Vec2(C.x + R * std::cos(ang), C.y + R * std::sin(ang)));
		}
	}

	// ============================================================================
	// Безопасная попытка «обновить» вид (в некоторых SDK нет этих идентификаторов)
	// ============================================================================
	static inline void TryRebuildRedraw()
	{
#if defined(APIDo_Rebuild) && defined(APIDo_Redraw)
		ACAPI_Automate(APIDo_Rebuild, nullptr, nullptr);
		ACAPI_Automate(APIDo_Redraw, nullptr, nullptr);
#endif
	}

	// ============================================================================
	// ShapePrims → собираем контуры (берём самый большой по площади)
	// ============================================================================
	static std::vector<std::vector<Vec2>>* g_shapePolys = nullptr;

	static GSErrCode __ACENV_CALL ShapePrimsCollector(const API_PrimElement* prim,
		const void* par1, const void* par2, const void* par3)
	{
		if (g_shapePolys == nullptr || prim == nullptr) return NoError;

		if (prim->header.typeID == API_PrimPolyID) {
			const API_PrimPoly& info = prim->poly;
			const API_Coord* coords = reinterpret_cast<const API_Coord*>    (par1);
			const Int32* pends = reinterpret_cast<const Int32*>        (par2);
			const API_PolyArc* parcs = reinterpret_cast<const API_PolyArc*>  (par3);

			if (coords == nullptr || pends == nullptr || info.nCoords <= 1 || info.nSubPolys <= 0)
				return NoError;

			// Обрабатываем ВСЕ контуры, не только внешний
			for (Int32 sub = 0; sub < info.nSubPolys; ++sub) {
				const Int32 begInd = pends[sub] + 1;
				const Int32 endInd = pends[sub + 1];

				std::vector<Vec2> ring;
				ring.reserve(std::max(0, endInd - begInd + 1));
				ring.emplace_back(coords[begInd]);

				for (Int32 i = begInd; i < endInd; ++i) {
					const int aIdx = FindArcRecord(parcs, info.nArcs, i);
					if (aIdx >= 0) {
						// Обрабатываем дугу с повышенной точностью
						AppendArcApprox(coords[i], coords[i + 1], parcs[aIdx].arcAngle, ring, 0.05);
					} else {
						ring.emplace_back(coords[i + 1]);
					}
				}

				if (!ring.empty()) g_shapePolys->push_back(std::move(ring));
			}
		}

		return NoError;
	}

	// точный контур элемента из план-проекции (либо fallback из memo/ref-line)
	static bool GetElementContour(const API_Guid& guid, std::vector<Vec2>& contour)
	{
		contour.clear();

		// если доступны соответствующие идентификаторы — дернём «обновление» вида
		TryRebuildRedraw();

		std::vector<std::vector<Vec2>> cands;
		g_shapePolys = &cands;

		API_Elem_Head head = {}; head.guid = guid;
		GSErrCode e = ACAPI_DrawingPrimitive_ShapePrims(head, ShapePrimsCollector);
		g_shapePolys = nullptr;

		if (e == NoError && !cands.empty()) {
			// Ищем ВНЕШНИЙ контур (самый большой по площади)
			size_t best = 0; double bestA = -1.0;
			for (size_t i = 0; i < cands.size(); ++i) {
				double a = PolygonArea(cands[i]);
				Log(GS::UniString::Printf("Candidate %d: area=%.3f, pts=%d", (int)i, a, (int)cands[i].size()));
				if (a > bestA) { bestA = a; best = i; }
			}
			contour = std::move(cands[best]);
			Log(GS::UniString::Printf("ShapePrims EXTERNAL contour: %d pts (area=%.3f, %d candidates)", 
				(int)contour.size(), bestA, (int)cands.size()));
			return true;
		}
		
		Log("ShapePrims failed, trying fallback...");

		// ------ fallback ----------------------------------------------------------
		API_Element elem = {}; elem.header.guid = guid;
		if (ACAPI_Element_Get(&elem) != NoError) {
			Log("Failed to get element " + APIGuidToString(guid));
			return false;
		}

		const API_ElemTypeID tid = elem.header.type.typeID;

		if (tid == API_WallID) {          // реф-линия
			// Для многосекционных стен получаем полную линию привязки
			API_ElementMemo memo = {};
			GSErrCode err = ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_Polygon);
			if (err == NoError && memo.coords != nullptr) {
				// Используем все точки линии привязки
				Int32 nCoords = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) - 1;
				for (Int32 i = 1; i <= nCoords; ++i) {
					contour.emplace_back((*memo.coords)[i]);
				}
				ACAPI_DisposeElemMemoHdls(&memo);
				Log(GS::UniString::Printf("Wall contour: %d points (full ref line)", nCoords));
				return true;
			}
			ACAPI_DisposeElemMemoHdls(&memo);
			
			// Fallback к простой линии
			contour.emplace_back(elem.wall.begC);
			contour.emplace_back(elem.wall.endC);
			Log("Wall contour: simple ref line");
			return true;
		}

		API_ElementMemo memo = {};
		e = ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_Polygon);
		if (e != NoError || memo.coords == nullptr) {
			Log("Fallback memo failed (no coords)");
			ACAPI_DisposeElemMemoHdls(&memo);
			return false;
		}

		// Получаем контур как в LandscapeHelper - через BuildFromPolyMemo
		Int32 nSub = (memo.pends != nullptr) ? (Int32)(BMGetHandleSize((GSHandle)memo.pends) / sizeof(Int32)) - 1 : 0;
		Int32 nArcs = (memo.parcs != nullptr) ? (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc)) : 0;
		
		Log(GS::UniString::Printf("Fallback: nSub=%d, nArcs=%d", nSub, nArcs));
		
		if (nSub > 0) {
			// Множественные контуры - ищем внешний с дугами
			std::vector<std::vector<Vec2>> allContours;
			for (Int32 sub = 0; sub < nSub; ++sub) {
				Int32 beg = (*memo.pends)[sub] + 1;
				Int32 end = (*memo.pends)[sub + 1];
				std::vector<Vec2> ring;
				ring.emplace_back((*memo.coords)[beg]);
				
				// Обрабатываем сегменты с дугами
				for (Int32 i = beg; i < end; ++i) {
					// Ищем дугу для этого сегмента
					bool hasArc = false;
					if (nArcs > 0 && memo.parcs != nullptr) {
						for (Int32 a = 0; a < nArcs; ++a) {
							if ((*memo.parcs)[a].begIndex == i) {
								// Найдена дуга - аппроксимируем
								AppendArcApprox((*memo.coords)[i], (*memo.coords)[i + 1], 
									(*memo.parcs)[a].arcAngle, ring, 0.05);
								hasArc = true;
								break;
							}
						}
					}
					if (!hasArc) {
						// Обычный сегмент
						ring.emplace_back((*memo.coords)[i + 1]);
					}
				}
				if (!ring.empty()) allContours.push_back(std::move(ring));
			}
			
			// Выбираем самый большой по площади
			if (!allContours.empty()) {
				size_t best = 0; double bestA = -1.0;
				for (size_t i = 0; i < allContours.size(); ++i) {
					double a = PolygonArea(allContours[i]);
					Log(GS::UniString::Printf("Fallback contour %d: area=%.3f, pts=%d", (int)i, a, (int)allContours[i].size()));
					if (a > bestA) { bestA = a; best = i; }
				}
				contour = std::move(allContours[best]);
				Log(GS::UniString::Printf("Fallback EXTERNAL contour with arcs: %d pts (area=%.3f)", (int)contour.size(), bestA));
			}
		} else {
			// Один контур с дугами
			Int32 nCoords = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) - 1;
			contour.emplace_back((*memo.coords)[1]);
			
			for (Int32 i = 1; i < nCoords; ++i) {
				// Ищем дугу для этого сегмента
				bool hasArc = false;
				if (nArcs > 0 && memo.parcs != nullptr) {
					for (Int32 a = 0; a < nArcs; ++a) {
						if ((*memo.parcs)[a].begIndex == i) {
							// Найдена дуга - аппроксимируем
							AppendArcApprox((*memo.coords)[i], (*memo.coords)[i + 1], 
								(*memo.parcs)[a].arcAngle, contour, 0.05);
							hasArc = true;
							break;
						}
					}
				}
				if (!hasArc) {
					// Обычный сегмент
					contour.emplace_back((*memo.coords)[i + 1]);
				}
			}
			Log(GS::UniString::Printf("Fallback single contour with arcs: %d pts", (int)contour.size()));
		}

		ACAPI_DisposeElemMemoHdls(&memo);
		Log(GS::UniString::Printf("Fallback contour: %d pts", (int)contour.size()));
		return !contour.empty();
	}

	// ============================================================================
	// Пересечения с одним контуром: ближайшее по заданному лучу
	// ============================================================================
	static bool NearestRayIntersection(const Vec2& origin, const Vec2& dirUnit,
		const std::vector<Vec2>& poly,
		Vec2& hit, double& outDist)
	{
		const size_t n = poly.size();
		if (n < 2) return false;

		const bool closed = (n > 2);

		bool   found = false;
		double bestD = std::numeric_limits<double>::max();
		Vec2   bestHit;

		for (size_t i = 0; i < n; ++i) {
			const Vec2& A = poly[i];
			const Vec2& B = closed ? poly[(i + 1) % n] : poly[i + 1];
			if (!closed && i + 1 >= n) break;

			double t, d;
			if (RaySegmentIntersection(origin, dirUnit, A, B, t, d)) {
				if (t >= -1e-12 && d < bestD) {
					bestD = d;
					bestHit = origin + dirUnit * t;
					found = true;
				}
			}
		}

		if (found) {
			hit = bestHit;
			outDist = bestD;
		}
		return found;
	}

	// ============================================================================
	// Агрегатор по ВСЕМ контурам: самое ДАЛЬНЕЕ попадание на данной стороне
	// ============================================================================
	static bool FarthestHitOnSide(const Vec2& origin, const Vec2& sideDirUnit,
		const std::vector<std::vector<Vec2>>& contours,
		Vec2& farHit, double& farDist)
	{
		bool   ok = false;
		double maxD = -1.0;
		Vec2   bestH;

		for (const auto& c : contours) {
			Vec2 h; double d;
			if (NearestRayIntersection(origin, sideDirUnit, c, h, d)) {
				// Проверяем что пересечение ВНЕШНЕЕ (в направлении от центра к origin)
				Vec2 center = {0, 0};
				for (const Vec2& pt : c) {
					center.x += pt.x;
					center.y += pt.y;
				}
				center.x /= c.size();
				center.y /= c.size();
				
				// Вектор от центра к origin
				Vec2 toOrigin = (origin - center).normalized();
				// Вектор от центра к пересечению
				Vec2 toHit = (h - center).normalized();
				
				// Скалярное произведение: если > 0, то пересечение в том же направлении (внешнее)
				double dot = toOrigin.x * toHit.x + toOrigin.y * toHit.y;
				
				if (dot > 0.1) { // Внешнее пересечение
					if (d > maxD) { maxD = d; bestH = h; ok = true; }
				}
			}
		}
		if (ok) { farDist = maxD; farHit = bestH; }
		return ok;
	}

	// ============================================================================
	// Создание размера между двумя точками (любой угол) с нулевым зазором свидетеля
	// ============================================================================
	static bool CreateDimensionBetweenPoints(const API_Coord& pt1, const API_Coord& pt2)
	{
		const double dx = pt2.x - pt1.x;
		const double dy = pt2.y - pt1.y;
		const double len = std::hypot(dx, dy);
		if (len < 1e-6) { Log("Dimension too small"); return false; }

		Vec2 A(pt1), B(pt2);
		Vec2 u = (B - A).normalized();  // направление размера
		Vec2 n = u.perpendicular();     // нормаль влево
		const double offset = 0.50;      // отступ базовой (м)

		API_Element dim = {}; dim.header.type = API_DimensionID;
		GSErrCode err = ACAPI_Element_GetDefaults(&dim, nullptr);
		if (err != NoError) { Log("GetDefaults for Dimension failed"); return false; }

		// нулевой зазор выносной линии
		dim.dimension.defWitnessForm = APIWtn_Fix;
		dim.dimension.defWitnessVal = 0.0;

		dim.dimension.dimAppear = APIApp_Normal;
		dim.dimension.textPos = APIPos_Above;
		dim.dimension.textWay = APIDir_Parallel;

		// базовая линия (проходит через refC, направлена как u)
		Vec2 ref = A + n * offset;
		dim.dimension.refC.x = ref.x;
		dim.dimension.refC.y = ref.y;
		dim.dimension.direction.x = (B.x - A.x);
		dim.dimension.direction.y = (B.y - A.y);

		// проекция точки P на базовую линию
		auto footOnBaseline = [&](const Vec2& P) -> API_Coord {
			double t = (P - ref).dot(u);
			Vec2   F = ref + u * t;
			return F.toCoord();
			};

		API_ElementMemo memo = {}; BNZeroMemory(&memo, sizeof(API_ElementMemo));
		dim.dimension.nDimElem = 2;
		memo.dimElems = reinterpret_cast<API_DimElem**> (
			BMAllocateHandle(2 * sizeof(API_DimElem), ALLOCATE_CLEAR, 0));
		if (memo.dimElems == nullptr) { Log("Memory allocation failed for dimElems"); return false; }

		API_DimElem& e1 = (*memo.dimElems)[0];
		e1.base.loc = pt1; e1.base.base.line = false; e1.base.base.special = false;
		e1.pos = footOnBaseline(A);

		API_DimElem& e2 = (*memo.dimElems)[1];
		e2.base.loc = pt2; e2.base.base.line = false; e2.base.base.special = false;
		e2.pos = footOnBaseline(B);

		err = ACAPI_Element_Create(&dim, &memo);
		ACAPI_DisposeElemMemoHdls(&memo);

		if (err != NoError) {
			Log(GS::UniString::Printf("Dimension creation failed: %d", (int)err));
			return false;
		}
		return true;
	}

	// ============================================================================
	// Публичные функции
	// ============================================================================
	bool SetMarkupStep(double stepMM)
	{
		if (stepMM <= 0.0) { Log("Invalid step: must be > 0"); return false; }
		g_stepMeters = stepMM / 1000.0;
		Log(GS::UniString::Printf("Step set: %.1f mm (%.6f m)", stepMM, g_stepMeters));
		return true;
	}

	bool CreateMarkupDimensions()
	{
		Log("=== CreateMarkupDimensions START ===");

		// 1) выделение
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) { Log("No elements selected"); return false; }
		Log(GS::UniString::Printf("Selected elements: %d", (int)selNeigs.GetSize()));

		struct ElementContourData { API_Guid guid; std::vector<Vec2> contour; };
		std::vector<ElementContourData> elements;

		for (const API_Neig& n : selNeigs) {
			API_Element h = {}; h.header.guid = n.guid;
			if (ACAPI_Element_GetHeader(&h.header) != NoError) continue;

			const API_ElemTypeID tid = h.header.type.typeID;
			if (tid != API_MeshID && tid != API_SlabID && tid != API_WallID && tid != API_ShellID)
				continue;

			ElementContourData d; d.guid = n.guid;
			if (GetElementContour(n.guid, d.contour) && !d.contour.empty())
				elements.push_back(std::move(d));
		}

		if (elements.empty()) { Log("No supported elements (Mesh/Slab/Wall/Shell) in selection"); return false; }
		Log(GS::UniString::Printf("Valid elements for markup: %d", (int)elements.size()));

		// 2) направление
		API_GetPointType gp1 = {}; CHCopyC("Разметка: укажите НАЧАЛО направления (точка 1)", gp1.prompt);
		GSErrCode err = ACAPI_UserInput_GetPoint(&gp1);
		if (err != NoError) { Log(GS::UniString::Printf("GetPoint #1 cancelled/failed: %d", (int)err)); return false; }

		API_GetPointType gp2 = {}; CHCopyC("Разметка: укажите КОНЕЦ направления (точка 2)", gp2.prompt);
		err = ACAPI_UserInput_GetPoint(&gp2);
		if (err != NoError) { Log(GS::UniString::Printf("GetPoint #2 cancelled/failed: %d", (int)err)); return false; }

		const Vec2 P1(gp1.pos.x, gp1.pos.y);
		const Vec2 P2(gp2.pos.x, gp2.pos.y);
		const Vec2 direction = (P2 - P1).normalized();
		const Vec2 perpendicular = direction.perpendicular();
		const double lineLength = (P2 - P1).length();

		Log(GS::UniString::Printf("Point 1: (%.6f, %.6f)", P1.x, P1.y));
		Log(GS::UniString::Printf("Point 2: (%.6f, %.6f)", P2.x, P2.y));
		Log(GS::UniString::Printf("Direction vector: (%.3f, %.3f)", direction.x, direction.y));
		Log(GS::UniString::Printf("Perpendicular vector: (%.3f, %.3f)", perpendicular.x, perpendicular.y));
		Log(GS::UniString::Printf("Direction line: P1(%.2f, %.2f) → P2(%.2f, %.2f), length=%.2fm",
			P1.x, P1.y, P2.x, P2.y, lineLength));

		// Список контуров для агрегатора
		std::vector<std::vector<Vec2>> contours; contours.reserve(elements.size());
		for (const auto& e : elements) contours.push_back(e.contour);

		// 3) определяем глобальную сторону (+⊥ или -⊥) и первое попадание.
		Vec2 firstHit; double firstTOnLine = -1.0; int sideSign = 0;

		for (double t = 0.0; t <= lineLength + 1e-9; t += std::max(0.05, g_stepMeters * 0.1)) {
			const Vec2 origin = P1 + direction * t;

			Vec2 hit; double d;

			if (FarthestHitOnSide(origin, perpendicular, contours, hit, d)) { firstHit = hit; firstTOnLine = t; sideSign = +1; break; }
			if (FarthestHitOnSide(origin, (perpendicular * -1.0), contours, hit, d)) { firstHit = hit; firstTOnLine = t; sideSign = -1; break; }
		}
		if (sideSign == 0) { Log("No intersection found with any element contour"); return false; }

		Log(GS::UniString::Printf("First hit at t=%.3f, side=%s", firstTOnLine, sideSign > 0 ? "+⊥" : "-⊥"));

		// 4) шаги вдоль линии и попадания только на выбранной стороне.
		std::vector<std::pair<Vec2, Vec2>> dimensionPairs;
		dimensionPairs.push_back({ P1 + direction * firstTOnLine, firstHit });

		const Vec2 sideDir = (sideSign > 0 ? perpendicular : perpendicular * -1.0);

		for (double t = firstTOnLine + g_stepMeters; t <= lineLength + 1e-9; t += g_stepMeters) {
			const Vec2 origin = P1 + direction * t;

			Vec2 hit; double d;
			if (FarthestHitOnSide(origin, sideDir, contours, hit, d)) {
				dimensionPairs.push_back({ origin, hit });
				Log(GS::UniString::Printf("Pair t=%.3f: (%.3f,%.3f) → (%.3f,%.3f)",
					t, origin.x, origin.y, hit.x, hit.y));
			}
		}

		Log(GS::UniString::Printf("Total dimension pairs: %d", (int)dimensionPairs.size()));

		// 5) Undo-группа
		int createdCount = 0;
		err = ACAPI_CallUndoableCommand("Разметка", [&]() -> GSErrCode {
			for (const auto& pr : dimensionPairs) {
				const Vec2& A = pr.first;
				const Vec2& B = pr.second;
				const double d = std::hypot(B.x - A.x, B.y - A.y);
				if (d > 0.01) {
					if (CreateDimensionBetweenPoints(A.toCoord(), B.toCoord())) {
						++createdCount;
						Log(GS::UniString::Printf("Dimension created: distance=%.3fm", d));
					}
				}
			}
			return NoError;
			});

		if (err == NoError && createdCount > 0) {
			Log(GS::UniString::Printf("=== SUCCESS: Created %d dimensions ===", createdCount));
			return true;
		}
		else if (createdCount == 0) {
			Log("No dimensions created (no intersections found)");
			return false;
		}
		else {
			Log(GS::UniString::Printf("Undo command failed: err=%d", (int)err));
			return false;
		}
	}

} // namespace MarkupHelper
