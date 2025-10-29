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
	// Constants
	// ============================================================================
	constexpr double PI = 3.14159265358979323846;

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
		// ACAPI_WriteReport("[Markup] %s", false, msg.ToCStr().Get());
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
	// Структура сегмента контура (адаптировано из LandscapeHelper)
	// ============================================================================
	struct ContourSeg {
		enum Kind { Line, Arc } kind;
		Vec2 a{}, b{};        // Line: начало и конец
		Vec2 c{};             // Arc: центр
		double r = 0.0;       // Arc: радиус
		double a0 = 0.0;      // Arc: начальный угол
		double a1 = 0.0;      // Arc: конечный угол
		double L = 0.0;       // длина сегмента
	};

	// ============================================================================
	// Нормализация угла к диапазону [0, 2π)
	// ============================================================================
	static inline double Norm2PI(double a) {
		const double twoPi = 2.0 * PI;
		while (a < 0.0) a += twoPi;
		while (a >= twoPi) a -= twoPi;
		return a;
	}

	// ============================================================================
	// Вычисление CCW-дельты между углами (адаптировано из LandscapeHelper)
	// ============================================================================
	static inline double CCWDelta(double a0, double a1) {
		a0 = Norm2PI(a0);
		a1 = Norm2PI(a1);
		double d = a1 - a0;
		if (d < 0.0) d += 2.0 * PI;
		return d; // [0, 2π)
	}

	// ============================================================================
	// Построение контура из memo с правильной обработкой дуг (адаптировано из LandscapeHelper)
	// ============================================================================
	static void BuildContourFromMemo(std::vector<ContourSeg>& out, API_ElementMemo& memo)
	{
		if (memo.coords == nullptr) return;

		const Int32 nAll = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord));
		const Int32 nPts = std::max<Int32>(0, nAll - 1); // валидные точки: 1..nPts
		if (nPts < 2) return;

		// Концы цепочек (для многоконтурных полигонов)
		std::vector<Int32> ends;
		if (memo.pends != nullptr) {
			const Int32 nEnds = (Int32)(BMGetHandleSize((GSHandle)memo.pends) / sizeof(Int32));
			for (Int32 k = 0; k < nEnds; ++k) {
				const Int32 ind = (*memo.pends)[k];
				if (ind >= 1 && ind <= nPts) ends.push_back(ind);
			}
		}
		if (ends.empty()) ends.push_back(nPts); // одна замкнутая цепочка

		auto isEnd = [&](Int32 i) -> bool {
			return std::find(ends.begin(), ends.end(), i) != ends.end();
		};

		// Карта дуг по begIndex
		std::vector<double> arcByBeg(nPts + 1, 0.0);
		if (memo.parcs != nullptr) {
			const Int32 nArcs = (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc));
			for (Int32 k = 0; k < nArcs; ++k) {
				const API_PolyArc& pa = (*memo.parcs)[k];
				if (pa.begIndex >= 1 && pa.begIndex <= nPts - 1)
					arcByBeg[pa.begIndex] = pa.arcAngle; // со знаком
			}
		}

		// Обрабатываем все рёбра контура
		for (Int32 i = 1; i <= nPts - 1; ++i) {
			if (isEnd(i)) continue; // не соединяем через конец цепочки

			const Int32 j = i + 1;
			const Vec2 A((*memo.coords)[i]);
			const Vec2 B((*memo.coords)[j]);

			const double angArc = arcByBeg[i];
			
			// Если дуга отсутствует или угол близок к нулю - это прямая линия
			if (std::fabs(angArc) < 1e-9) {
				ContourSeg seg;
				seg.kind = ContourSeg::Line;
				seg.a = A;
				seg.b = B;
				seg.L = (B - A).length();
				if (seg.L > 1e-9) out.push_back(seg);
				continue;
			}

			// Обрабатываем дугу: находим центр и радиус
			const double dx = B.x - A.x, dy = B.y - A.y;
			const double chord = std::hypot(dx, dy);
			if (chord < 1e-9) continue;

			// Радиус дуги по формуле из LandscapeHelper
			const double r = std::fabs(chord / (2.0 * std::sin(std::fabs(angArc) * 0.5)));
			
			// Середина хорды
			const double mx = (A.x + B.x) * 0.5, my = (A.y + B.y) * 0.5;
			
			// Нормаль к хорде
			const double nx = -dy / chord, ny = dx / chord;
			
			// Расстояние от середины хорды до центра
			const double d = std::sqrt(std::max(r * r - 0.25 * chord * chord, 0.0));

			// Две возможные окружности - выбираем ту, у которой sweep ближе к arcAngle
			struct Cand { Vec2 c; double a0, a1, L; };
			auto makeCand = [&](double sx, double sy) -> Cand {
				const double cx = mx + sx * d, cy = my + sy * d;
				const double aA = std::atan2(A.y - cy, A.x - cx);
				const double aB = std::atan2(B.y - cy, B.x - cx);
				double sweep = (angArc > 0.0) ? CCWDelta(aA, aB) : -CCWDelta(aB, aA);
				Cand cnd;
				cnd.c = Vec2(cx, cy);
				cnd.a0 = aA;
				cnd.a1 = aA + sweep;
				cnd.L = r * std::fabs(sweep);
				return cnd;
			};

			const Cand c1 = makeCand(nx, ny);
			const Cand c2 = makeCand(-nx, -ny);

			const double d1 = std::fabs((c1.a1 - c1.a0) - angArc);
			const double d2 = std::fabs((c2.a1 - c2.a0) - angArc);
			
			// ИСПРАВЛЕНИЕ: выбираем центр дуги по минимальной разности (как в LandscapeHelper)
			const Cand& best = (d1 <= d2 ? c1 : c2);
			
			// Отладочная информация для дуг
			// Log(GS::UniString::Printf("Arc analysis: angArc=%.6f, c1_sweep=%.6f (diff=%.6f), c2_sweep=%.6f (diff=%.6f), chosen=%s (by diff)", 
			//	angArc, (c1.a1 - c1.a0), d1, (c2.a1 - c2.a0), d2, (d1 <= d2 ? "c1" : "c2")));
			// Log(GS::UniString::Printf("Arc center: (%.3f,%.3f), r=%.3f, angles=%.3f→%.3f, sweep=%.3f", 
			//	best.c.x, best.c.y, r, best.a0, best.a1, (best.a1 - best.a0)));

			ContourSeg seg;
			seg.kind = ContourSeg::Arc;
			seg.c = best.c;
			seg.r = r;
			seg.a0 = best.a0;
			seg.a1 = best.a1;
			seg.L = best.L;
			if (seg.L > 1e-9) out.push_back(seg);
		}
	}

	// ============================================================================
	// Получение контура элемента как сегментов (линии + дуги) через memo
	// ============================================================================
	static bool GetElementContourSegments(const API_Guid& guid, std::vector<ContourSeg>& segments)
	{
		segments.clear();

		API_Element elem = {};
		elem.header.guid = guid;
		if (ACAPI_Element_Get(&elem) != NoError) {
			// Log("Failed to get element " + APIGuidToString(guid));
			return false;
		}

		const API_ElemTypeID tid = elem.header.type.typeID;

		// Для стен - получаем полный контур через memo (включая дуги)
		if (tid == API_WallID) {
			API_ElementMemo memo = {};
			GSErrCode e = ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_Polygon);
			
			if (e == NoError && memo.coords != nullptr) {
				// Строим контур с правильной обработкой дуг
				BuildContourFromMemo(segments, memo);
				
				const Int32 nPts = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) - 1;
				const Int32 nArcs = memo.parcs ? (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc)) : 0;
				
				// Log(GS::UniString::Printf("Wall contour built: %d segments from %d points, %d arcs", 
				//	(int)segments.size(), (int)nPts, (int)nArcs));
				
				// Отладочная информация о сегментах стены
				int lineCount = 0, arcCount = 0;
				for (const ContourSeg& seg : segments) {
					if (seg.kind == ContourSeg::Line) {
						lineCount++;
						// Log(GS::UniString::Printf("Wall line segment: (%.3f,%.3f) → (%.3f,%.3f), L=%.3f", 
						//	seg.a.x, seg.a.y, seg.b.x, seg.b.y, seg.L));
					} else {
						arcCount++;
						// Log(GS::UniString::Printf("Wall arc segment: center(%.3f,%.3f), r=%.3f, angles=%.3f→%.3f, L=%.3f", 
						//	seg.c.x, seg.c.y, seg.r, seg.a0, seg.a1, seg.L));
					}
				}
				// Log(GS::UniString::Printf("Wall segments: %d lines, %d arcs", lineCount, arcCount));
				
				ACAPI_DisposeElemMemoHdls(&memo);
				return !segments.empty();
			}
			
			// Fallback: если не удалось получить memo, используем реф-линию
			ACAPI_DisposeElemMemoHdls(&memo);
			ContourSeg seg;
			seg.kind = ContourSeg::Line;
			seg.a = Vec2(elem.wall.begC);
			seg.b = Vec2(elem.wall.endC);
			seg.L = (seg.b - seg.a).length();
			segments.push_back(seg);
			// Log("Wall contour: 1 segment (ref line fallback)");
			return true;
		}

		// Для остальных элементов (Mesh, Slab, Shell) - получаем контур через memo
		API_ElementMemo memo = {};
		GSErrCode e = ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_Polygon);
		
		if (e == NoError && memo.coords != nullptr) {
			// Строим контур с правильной обработкой дуг
			BuildContourFromMemo(segments, memo);
			
			const Int32 nPts = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) - 1;
			const Int32 nArcs = memo.parcs ? (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / sizeof(API_PolyArc)) : 0;
			
			// Log(GS::UniString::Printf("Contour built: %d segments from %d points, %d arcs", 
			//	(int)segments.size(), (int)nPts, (int)nArcs));
			
			// Отладочная информация о сегментах
			int lineCount = 0, arcCount = 0;
			for (const ContourSeg& seg : segments) {
				if (seg.kind == ContourSeg::Line) {
					lineCount++;
					// Log(GS::UniString::Printf("Line segment: (%.3f,%.3f) → (%.3f,%.3f), L=%.3f", 
					//	seg.a.x, seg.a.y, seg.b.x, seg.b.y, seg.L));
				} else {
					arcCount++;
					// Log(GS::UniString::Printf("Arc segment: center(%.3f,%.3f), r=%.3f, angles=%.3f→%.3f, L=%.3f", 
					//	seg.c.x, seg.c.y, seg.r, seg.a0, seg.a1, seg.L));
				}
			}
			// Log(GS::UniString::Printf("Segments: %d lines, %d arcs", lineCount, arcCount));
			
			ACAPI_DisposeElemMemoHdls(&memo);
			return !segments.empty();
		}
		
		ACAPI_DisposeElemMemoHdls(&memo);
		// Log("Failed to get element contour from memo");
			return false;
		}



	// ============================================================================
	// Вычисление точки на контуре по параметру расстояния (адаптировано из EvalOnPath)
	// ============================================================================
	static void EvalOnContour(const std::vector<ContourSeg>& segments, double s, Vec2* outP, double* outTanAngleRad)
	{
		double acc = 0.0;
		for (const ContourSeg& seg : segments) {
			if (s > acc + seg.L) { acc += seg.L; continue; }
			const double f = (seg.L < 1e-9) ? 0.0 : (s - acc) / seg.L;

			if (seg.kind == ContourSeg::Line) {
				if (outP)           *outP = seg.a + (seg.b - seg.a) * f;
				if (outTanAngleRad) *outTanAngleRad = std::atan2(seg.b.y - seg.a.y, seg.b.x - seg.a.x);
			}
			else {
				// Для дуг - правильная параметризация (скопировано из EvalOnPath)
				const double sweep = seg.a1 - seg.a0;  // со знаком!
				const double ang = seg.a0 + f * sweep;
				if (outP)           *outP = Vec2(seg.c.x + seg.r * std::cos(ang), seg.c.y + seg.r * std::sin(ang));
				if (outTanAngleRad) *outTanAngleRad = ang + ((sweep >= 0.0) ? +PI / 2.0 : -PI / 2.0);
			}
			return;
		}
		// Если s выходит за пределы - берем последнюю точку
		const ContourSeg& seg = segments.back();
		if (seg.kind == ContourSeg::Line) {
			if (outP)           *outP = seg.b;
			if (outTanAngleRad) *outTanAngleRad = std::atan2(seg.b.y - seg.a.y, seg.b.x - seg.a.x);
		}
		else {
			if (outP)           *outP = Vec2(seg.c.x + seg.r * std::cos(seg.a1), seg.c.y + seg.r * std::sin(seg.a1));
			if (outTanAngleRad) *outTanAngleRad = seg.a1 + ((seg.a1 - seg.a0 >= 0.0) ? +PI / 2.0 : -PI / 2.0);
		}
	}

	// ============================================================================
	// Поиск пересечения луча с дугой (возвращает БЛИЖАЙШЕЕ пересечение)
	// ============================================================================
	static bool RayArcIntersection(const Vec2& origin, const Vec2& dirUnit,
		const Vec2& center, double radius, double a0, double a1,
		Vec2& intersection, double& distance)
	{
		// Уравнение луча: P = origin + t * dirUnit
		// Уравнение окружности: (x - cx)² + (y - cy)² = r²
		// Подставляем: (ox + t*dx - cx)² + (oy + t*dy - cy)² = r²
		
		const double dx = dirUnit.x, dy = dirUnit.y;
		const double ox = origin.x, oy = origin.y;
		const double cx = center.x, cy = center.y;
		
		// Квадратное уравнение: at² + bt + c = 0
		const double a = dx*dx + dy*dy;
		const double b = 2.0 * (dx*(ox - cx) + dy*(oy - cy));
		const double c = (ox - cx)*(ox - cx) + (oy - cy)*(oy - cy) - radius*radius;
		
		const double discriminant = b*b - 4.0*a*c;
		if (discriminant < 0.0) return false;
		
		const double sqrt_disc = std::sqrt(discriminant);
		const double t1 = (-b - sqrt_disc) / (2.0 * a);
		const double t2 = (-b + sqrt_disc) / (2.0 * a);
		
		// Ищем БЛИЖАЙШЕЕ пересечение в правильном направлении
		double bestT = std::numeric_limits<double>::max();
		bool found = false;
		
		for (double t : {t1, t2}) {
			if (t < 1e-12) continue; // пересечение в неправильном направлении
			
			Vec2 point = origin + dirUnit * t;
			double angle = std::atan2(point.y - cy, point.x - cx);
			// Проверяем принадлежность ТОчКИ дуге по НАПРАВЛЕННОЙ длине дуги
			// Используем тот же критерий, что и в LandscapeHelper: для CCW берём CCWDelta(a0, angle),
			// для CW берём CCWDelta(angle, a0). Значение должно лежать в [0, |sweep|].
			double sweep = a1 - a0; // со знаком
			angle = Norm2PI(angle);
			double a0n = Norm2PI(a0);
			double a1n = Norm2PI(a1);
			double deltaOnArc = 0.0;
			if (sweep >= 0.0) {
				// дуга против часовой: от a0 к a1 по CCW
				deltaOnArc = CCWDelta(a0n, angle);
			} else {
				// дуга по часовой: от a0 к a1 по CW
				// используем CCWDelta(angle, a0n) для правильного направления
				deltaOnArc = CCWDelta(angle, a0n);
			}
			bool inRange = (deltaOnArc >= -1e-12 && deltaOnArc <= std::fabs(sweep) + 1e-12);
			
			// Отладочная информация для пересечений с дугой
			// Log(GS::UniString::Printf("Arc intersection check: t=%.3f, point=(%.3f,%.3f), angle=%.3f, deltaOnArc=%.3f, sweep=%.3f, inRange=%s", 
			//	t, point.x, point.y, angle, deltaOnArc, std::fabs(sweep), inRange ? "YES" : "NO"));
			
			if (inRange && t < bestT) {
				bestT = t;
				intersection = point;
				distance = t;
					found = true;
			}
		}
		return found;
	}

	// ============================================================================
	// Поиск ближайшего пересечения луча с контуром
	// ============================================================================
	static bool FindNearestContourIntersection(const Vec2& origin, const Vec2& sideDirUnit,
		const std::vector<ContourSeg>& segments,
		Vec2& intersection, double& distance)
	{
		double minT = std::numeric_limits<double>::max(); // Ищем ближайшее по ПАРАМЕТРУ t
		Vec2 bestIntersection;
		bool found = false;
		
		// Ограничиваем поиск разумным радиусом (50 метров)
		const double maxSearchRadius = 50.0;
		
		// Ищем пересечения со всеми сегментами контура
		for (size_t i = 0; i < segments.size(); ++i) {
			const ContourSeg& seg = segments[i];
			if (seg.kind == ContourSeg::Line) {
				// Линейный сегмент
				double t, d;
				if (RaySegmentIntersection(origin, sideDirUnit, seg.a, seg.b, t, d)) {
					if (t >= -1e-12 && t < minT && d <= maxSearchRadius) { // пересечение в нужном направлении
						Vec2 intersectionPoint = origin + sideDirUnit * t;
						
						// Проверяем, что пересечение на сегменте
						Vec2 AB = seg.b - seg.a;
						Vec2 AI = intersectionPoint - seg.a;
						double dot = AI.dot(AB);
						double AB_length_sq = AB.dot(AB);
						
						if (dot >= 0.0 && dot <= AB_length_sq) {
						// Log(GS::UniString::Printf("Line intersection #%d: t=%.3f, dist=%.3f, point=(%.3f,%.3f)", 
						//	(int)i, t, d, intersectionPoint.x, intersectionPoint.y));
							minT = t;
							bestIntersection = intersectionPoint;
							distance = d;
							found = true;
						}
					}
				}
			}
			else {
				// Дуга - ИСПРАВЛЕНИЕ: ищем пересечения в правильном направлении
				Vec2 hit; double d;
				if (RayArcIntersection(origin, sideDirUnit, seg.c, seg.r, seg.a0, seg.a1, hit, d)) {
					// Проверяем, что пересечение в правильном направлении (t >= 0)
					Vec2 toHit = hit - origin;
					double t = toHit.dot(sideDirUnit);
					
					if (t >= -1e-12 && t < minT && d <= maxSearchRadius) {
					// Log(GS::UniString::Printf("Arc intersection #%d: t=%.3f, dist=%.3f, point=(%.3f,%.3f), center=(%.3f,%.3f), r=%.3f", 
					//	(int)i, t, d, hit.x, hit.y, seg.c.x, seg.c.y, seg.r));
						minT = t;
						bestIntersection = hit;
						distance = d;
						found = true;
					}
				}
			}
		}
		
		if (found) {
			intersection = bestIntersection;
			return true;
		}
		return false;
	}


	// ============================================================================
	// Создание размера между двумя точками с умным позиционированием
	// ============================================================================
	static bool CreateDimensionBetweenPoints(const API_Coord& pt1, const API_Coord& pt2)
	{
		const double dx = pt2.x - pt1.x;
		const double dy = pt2.y - pt1.y;
		const double len = std::hypot(dx, dy);
		if (len < 1e-6) return false; // точки совпали

		API_Element dim = {};
		dim.header.type = API_DimensionID;

		GSErrCode err = ACAPI_Element_GetDefaults(&dim, nullptr);
		if (err != NoError) return false;

		// --- Вид и текст как у тебя было, можно оставить/подкрутить при желании ---
		dim.dimension.dimAppear = APIApp_Normal;
		dim.dimension.textPos = APIPos_Above;     // текст над линией
		dim.dimension.textWay = APIDir_Parallel;  // ориентировать вдоль A→B

		// --- КЛЮЧЕВОЕ: никакого смещения до базовой линии ---
		dim.dimension.defWitnessForm = APIWtn_Fix;  // фиксированный отступ
		dim.dimension.defWitnessVal = 0.0;         // отступ 0 => выносных нет по сути
		dim.dimension.clipOtherSide = true;        // можно оставить true/false — неважно при 0

		// --- Базовая линия проходит РОВНО через A и B ---
		dim.dimension.refC.x = pt1.x;
		dim.dimension.refC.y = pt1.y;
		dim.dimension.direction.x = dx;   // направление A→B
		dim.dimension.direction.y = dy;

		// --- Узлы размерной цепочки: кладём ТУДА ЖЕ, без проекций ---
		API_ElementMemo memo = {};
		BNZeroMemory(&memo, sizeof(API_ElementMemo));
		dim.dimension.nDimElem = 2;

		memo.dimElems = reinterpret_cast<API_DimElem**>(
			BMAllocateHandle(2 * sizeof(API_DimElem), ALLOCATE_CLEAR, 0)
			);
		if (memo.dimElems == nullptr) return false;

		API_DimElem& e1 = (*memo.dimElems)[0];
		e1.base.loc = pt1;
		e1.base.base.line = false;
		e1.base.base.special = false;
		e1.pos = pt1;  // <- на базовой линии

		API_DimElem& e2 = (*memo.dimElems)[1];
		e2.base.loc = pt2;
		e2.base.base.line = false;
		e2.base.base.special = false;
		e2.pos = pt2;  // <- на базовой линии

		err = ACAPI_Element_Create(&dim, &memo);
		ACAPI_DisposeElemMemoHdls(&memo);

		return (err == NoError);
	}

	// ============================================================================
	// Публичные функции
	// ============================================================================
	bool SetMarkupStep(double stepMM)
	{
		ACAPI_WriteReport("[MarkupHelper] SetMarkupStep called with stepMM=%.6f", false, stepMM);
		if (stepMM <= 0.0) { 
			ACAPI_WriteReport("[MarkupHelper] Invalid step: must be > 0", false);
			return false; 
		}
		g_stepMeters = stepMM / 1000.0;
		ACAPI_WriteReport("[MarkupHelper] Step set: %.1f mm (%.6f m)", false, stepMM, g_stepMeters);
		return true;
	}

	bool CreateMarkupDimensions()
	{
		// Log("=== CreateMarkupDimensions START ===");

		// 1) выделение
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) { /* Log("No elements selected"); */ return false; }
		// Log(GS::UniString::Printf("Selected elements: %d", (int)selNeigs.GetSize()));

		struct ElementContourData { API_Guid guid; std::vector<ContourSeg> segments; double totalLength; };
		std::vector<ElementContourData> elements;

		for (const API_Neig& n : selNeigs) {
			API_Element h = {}; h.header.guid = n.guid;
			if (ACAPI_Element_GetHeader(&h.header) != NoError) continue;

			const API_ElemTypeID tid = h.header.type.typeID;
			if (tid != API_MeshID && tid != API_SlabID && tid != API_WallID && tid != API_ShellID)
				continue;

			ElementContourData d; d.guid = n.guid;
			if (GetElementContourSegments(n.guid, d.segments) && !d.segments.empty()) {
				// Вычисляем общую длину контура
				d.totalLength = 0.0;
				for (const ContourSeg& seg : d.segments) {
					d.totalLength += seg.L;
				}
				elements.push_back(std::move(d));
			}
		}

		if (elements.empty()) { /* Log("No supported elements (Mesh/Slab/Wall/Shell) in selection"); */ return false; }
		// Log(GS::UniString::Printf("Valid elements for markup: %d", (int)elements.size()));

		// 2) направление
		API_GetPointType gp1 = {}; CHCopyC("Разметка: укажите НАЧАЛО направления (точка 1)", gp1.prompt);
		GSErrCode err = ACAPI_UserInput_GetPoint(&gp1);
		if (err != NoError) { /* Log(GS::UniString::Printf("GetPoint #1 cancelled/failed: %d", (int)err)); */ return false; }

		API_GetPointType gp2 = {}; CHCopyC("Разметка: укажите КОНЕЦ направления (точка 2)", gp2.prompt);
		err = ACAPI_UserInput_GetPoint(&gp2);
		if (err != NoError) { /* Log(GS::UniString::Printf("GetPoint #2 cancelled/failed: %d", (int)err)); */ return false; }

		const Vec2 P1(gp1.pos.x, gp1.pos.y);
		const Vec2 P2(gp2.pos.x, gp2.pos.y);
		const Vec2 direction = (P2 - P1).normalized();
		const Vec2 perpendicular = direction.perpendicular();
		const double lineLength = (P2 - P1).length();

		// Log(GS::UniString::Printf("Point 1: (%.6f, %.6f)", P1.x, P1.y));
		// Log(GS::UniString::Printf("Point 2: (%.6f, %.6f)", P2.x, P2.y));
		// Log(GS::UniString::Printf("Direction vector: (%.3f, %.3f)", direction.x, direction.y));
		// Log(GS::UniString::Printf("Perpendicular vector: (%.3f, %.3f)", perpendicular.x, perpendicular.y));
		// Log(GS::UniString::Printf("Direction line: P1(%.2f, %.2f) → P2(%.2f, %.2f), length=%.2fm",
		//	P1.x, P1.y, P2.x, P2.y, lineLength));

		// 3) определяем глобальную сторону (+⊥ или -⊥) и первое попадание.
		Vec2 firstHit; double firstTOnLine = -1.0; int sideSign = 0;

		// Ищем первое пересечение, пробуя разные точки на линии направления
		for (double t = 0.0; t <= lineLength + 1e-9; t += std::max(0.05, g_stepMeters)) {
			const Vec2 origin = P1 + direction * t;

			// Проверяем пересечения с контурами всех элементов
			for (const auto& e : elements) {
			Vec2 hit; double d;
				if (FindNearestContourIntersection(origin, perpendicular, e.segments, hit, d)) {
					firstHit = hit; firstTOnLine = t; sideSign = +1; break;
				}
				if (FindNearestContourIntersection(origin, (perpendicular * -1.0), e.segments, hit, d)) {
					firstHit = hit; firstTOnLine = t; sideSign = -1; break;
				}
			}
			if (sideSign != 0) break;
		}
		if (sideSign == 0) { /* Log("No intersection found with any element contour"); */ return false; }

		// Log(GS::UniString::Printf("First hit at t=%.3f, side=%s", firstTOnLine, sideSign > 0 ? "+⊥" : "-⊥"));

		// 4) Строго по шагу: размеры через каждый заданный шаг
		std::vector<std::pair<Vec2, Vec2>> dimensionPairs;
		dimensionPairs.push_back({ P1 + direction * firstTOnLine, firstHit });

		const Vec2 sideDir = (sideSign > 0 ? perpendicular : perpendicular * -1.0);

		// Простой цикл по шагу - строго через каждый g_stepMeters
		for (double t = firstTOnLine + g_stepMeters; t <= lineLength + 1e-9; t += g_stepMeters) {
			const Vec2 origin = P1 + direction * t;

			Vec2 hit; double d;
			bool found = false;
			
			// Ищем пересечения с контурами всех элементов
			for (const auto& e : elements) {
				if (FindNearestContourIntersection(origin, sideDir, e.segments, hit, d)) {
				dimensionPairs.push_back({ origin, hit });
					// Log(GS::UniString::Printf("Step pair t=%.3f: (%.3f,%.3f) → (%.3f,%.3f)",
					// t, origin.x, origin.y, hit.x, hit.y));
					found = true;
					break; // берем ближайшее пересечение
				}
			}
		}

		// Log(GS::UniString::Printf("Total dimension pairs: %d", (int)dimensionPairs.size()));

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
						// Log(GS::UniString::Printf("Dimension created: distance=%.3fm", d));
					}
				}
			}
			return NoError;
			});

		if (err == NoError && createdCount > 0) {
			// Log(GS::UniString::Printf("=== SUCCESS: Created %d dimensions ===", createdCount));
			return true;
		}
		else if (createdCount == 0) {
			// Log("No dimensions created (no intersections found)");
			return false;
		}
		else {
			// Log(GS::UniString::Printf("Undo command failed: err=%d", (int)err));
			return false;
		}
	}

	// ============================================================================
	// Проставить размеры от точек привязки объектов до линии
	// ============================================================================
	bool CreateDimensionsToLine()
	{
		// Log("=== CreateDimensionsToLine START ===");

		// 1) Получаем выделенные элементы
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) { 
			// Log("No elements selected"); 
			return false; 
		}
		// Log(GS::UniString::Printf("Selected elements: %d", (int)selNeigs.GetSize()));

		// Собираем точки привязки объектов
		struct ObjectAnchor {
			API_Guid guid;
			Vec2 anchor;
			GS::UniString typeName;
		};
		std::vector<ObjectAnchor> objects;

		for (const API_Neig& n : selNeigs) {
			API_Element elem = {};
			elem.header.guid = n.guid;
			if (ACAPI_Element_Get(&elem) != NoError) continue;

			const API_ElemTypeID tid = elem.header.type.typeID;
			ObjectAnchor obj;
			obj.guid = n.guid;

			// Определяем точку привязки в зависимости от типа элемента
			if (tid == API_ObjectID) {
				obj.anchor = Vec2(elem.object.pos.x, elem.object.pos.y);
				obj.typeName = "Object";
				objects.push_back(obj);
				// Log(GS::UniString::Printf("Object anchor: (%.3f, %.3f)", obj.anchor.x, obj.anchor.y));
			}
			else if (tid == API_ColumnID) {
				obj.anchor = Vec2(elem.column.origoPos.x, elem.column.origoPos.y);
				obj.typeName = "Column";
				objects.push_back(obj);
				// Log(GS::UniString::Printf("Column anchor: (%.3f, %.3f)", obj.anchor.x, obj.anchor.y));
			}
			else if (tid == API_LampID) {
				obj.anchor = Vec2(elem.lamp.pos.x, elem.lamp.pos.y);
				obj.typeName = "Lamp";
				objects.push_back(obj);
				// Log(GS::UniString::Printf("Lamp anchor: (%.3f, %.3f)", obj.anchor.x, obj.anchor.y));
			}
		}

		if (objects.empty()) {
			// Log("No supported elements (Object/Column/Lamp) in selection");
			return false;
		}
		// Log(GS::UniString::Printf("Valid objects for dimensioning: %d", (int)objects.size()));

		// 2) Запрашиваем направление линии (2 точки)
		API_GetPointType gp1 = {};
		CHCopyC("Размеры: укажите НАЧАЛО линии (точка 1)", gp1.prompt);
		GSErrCode err = ACAPI_UserInput_GetPoint(&gp1);
		if (err != NoError) {
			// Log(GS::UniString::Printf("GetPoint #1 cancelled/failed: %d", (int)err));
			return false;
		}

		API_GetPointType gp2 = {};
		CHCopyC("Размеры: укажите КОНЕЦ линии (точка 2)", gp2.prompt);
		err = ACAPI_UserInput_GetPoint(&gp2);
		if (err != NoError) {
			// Log(GS::UniString::Printf("GetPoint #2 cancelled/failed: %d", (int)err));
			return false;
		}

		const Vec2 P1(gp1.pos.x, gp1.pos.y);
		const Vec2 P2(gp2.pos.x, gp2.pos.y);
		const Vec2 lineDir = (P2 - P1).normalized();
		const double lineLength = (P2 - P1).length();

		if (lineLength < 1e-6) {
			// Log("Line is too short");
			return false;
		}

		// Log(GS::UniString::Printf("Line: P1(%.3f, %.3f) → P2(%.3f, %.3f), length=%.2fm",
		//	P1.x, P1.y, P2.x, P2.y, lineLength));

		// 3) Для каждого объекта находим проекцию на линию и создаём размер
		std::vector<std::pair<Vec2, Vec2>> dimensionPairs;

		for (const auto& obj : objects) {
			// Вектор от P1 к точке объекта
			Vec2 toObj = obj.anchor - P1;
			
			// Проекция на линию: параметр t на отрезке P1->P2
			double t = toObj.dot(lineDir);
			
			// Проекция точки на линию
			Vec2 projection = P1 + lineDir * t;
			
			// Расстояние от объекта до проекции
			double dist = (obj.anchor - projection).length();
			
			// Log(GS::UniString::Printf("%s: anchor(%.3f,%.3f) → projection(%.3f,%.3f), dist=%.3fm",
			//	obj.typeName.ToCStr().Get(), obj.anchor.x, obj.anchor.y, 
			//	projection.x, projection.y, dist));

			// Добавляем пару для создания размера (только если расстояние > 0)
			if (dist > 0.01) { // минимум 1 см
				dimensionPairs.push_back({ obj.anchor, projection });
			}
		}

		if (dimensionPairs.empty()) {
			// Log("No valid dimensions to create (all objects on the line)");
			return false;
		}

		// Log(GS::UniString::Printf("Dimensions to create: %d", (int)dimensionPairs.size()));

		// 4) Создаём размеры в Undo-группе
		int createdCount = 0;
		err = ACAPI_CallUndoableCommand("Проставить размеры", [&]() -> GSErrCode {
			for (const auto& pair : dimensionPairs) {
				if (CreateDimensionBetweenPoints(pair.first.toCoord(), pair.second.toCoord())) {
					++createdCount;
					const double d = (pair.first - pair.second).length();
					// Log(GS::UniString::Printf("Dimension created: distance=%.3fm", d));
				}
			}
			return NoError;
		});

		if (err == NoError && createdCount > 0) {
			// Log(GS::UniString::Printf("=== SUCCESS: Created %d dimensions ===", createdCount));
			return true;
		}
		else if (createdCount == 0) {
			// Log("No dimensions created");
			return false;
		}
		else {
			// Log(GS::UniString::Printf("Undo command failed: err=%d", (int)err));
			return false;
		}
	}


	// ============================================================================
	// Проставить размеры между объектами последовательно (1→2, 2→3, 3→4...)
	// ============================================================================
	bool CreateDimensionsBetweenObjects()
	{
		Log("=== CreateDimensionsBetweenObjects START ===");

		// 1) Получаем выделенные элементы
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		if (ACAPI_Selection_Get(&selInfo, &selNeigs, false) != NoError) {
			Log("ERROR: не удалось получить выделение");
			return false;
		}

		if (selNeigs.IsEmpty()) {
			Log("ERROR: ничего не выделено");
			return false;
		}

		Log(GS::UniString::Printf("Выделено элементов: %d", (int)selNeigs.GetSize()));

		// 2) Собираем точки привязки всех объектов
		struct ObjectPoint {
			API_Coord coord;
			API_Guid guid;
			GS::UniString typeName;
		};
		std::vector<ObjectPoint> objects;

		for (UIndex i = 0; i < selNeigs.GetSize(); ++i) {
			const API_Neig& neig = selNeigs[i];
			
			// Получаем элемент
			API_Element element = {};
			element.header.guid = neig.guid;
			if (ACAPI_Element_Get(&element) != NoError) {
				Log(GS::UniString::Printf("WARN: не удалось получить элемент %d", (int)i));
				continue;
			}

			// Определяем точку привязки в зависимости от типа элемента
			ObjectPoint obj;
			obj.guid = neig.guid;
			bool hasAnchor = false;

			switch (element.header.type.typeID) {
			case API_ObjectID:
				obj.coord = element.object.pos;
				obj.typeName = "Object";
				hasAnchor = true;
				break;
			case API_ColumnID:
				obj.coord = element.column.origoPos;
				obj.typeName = "Column";
				hasAnchor = true;
				break;
			case API_LampID:
				obj.coord = element.lamp.pos;
				obj.typeName = "Lamp";
				hasAnchor = true;
				break;
			case API_WindowID:
			case API_DoorID:
				// Окна и двери не нужны
				continue;
			default:
				Log(GS::UniString::Printf("WARN: неподдерживаемый тип элемента: %d", (int)element.header.type.typeID));
				continue;
			}

			if (hasAnchor) {
				objects.push_back(obj);
				Log(GS::UniString::Printf("Точка %d: (%.3f, %.3f) - %s", 
					(int)objects.size(), obj.coord.x, obj.coord.y, obj.typeName.ToCStr().Get()));
			}
		}

		if (objects.size() < 2) {
			Log("ERROR: нужно минимум 2 объекта для создания размеров");
			return false;
		}

		Log(GS::UniString::Printf("Собрано объектов: %d", (int)objects.size()));

		// 3) Сортируем объекты по координатам (сначала по X, потом по Y)
		std::sort(objects.begin(), objects.end(), [](const ObjectPoint& a, const ObjectPoint& b) {
			if (std::abs(a.coord.x - b.coord.x) < 1e-6) {
				return a.coord.y < b.coord.y; // если X одинаковые, сортируем по Y
			}
			return a.coord.x < b.coord.x; // сортируем по X
		});

		Log("Объекты отсортированы по координатам");

		// 4) Создаем размеры последовательно (1→2, 2→3, 3→4...)
		int createdCount = 0;
		GSErrCode err = ACAPI_CallUndoableCommand("Размеры между объектами", [&]() -> GSErrCode {
			for (size_t i = 0; i < objects.size() - 1; ++i) {
				const API_Coord& pt1 = objects[i].coord;
				const API_Coord& pt2 = objects[i + 1].coord;

				// Вычисляем расстояние
				double dx = pt2.x - pt1.x;
				double dy = pt2.y - pt1.y;
				double distance = sqrt(dx * dx + dy * dy);

				// Создаем размер между двумя точками используя существующую функцию
				if (CreateDimensionBetweenPoints(pt1, pt2)) {
					createdCount++;
					Log(GS::UniString::Printf("Размер %d: %.3fм между объектами %d→%d", 
						createdCount, distance, (int)i+1, (int)i+2));
				} else {
					Log(GS::UniString::Printf("ERROR: не удалось создать размер между объектами %d→%d", (int)i+1, (int)i+2));
				}
			}
			return NoError;
		});

		if (err == NoError && createdCount > 0) {
			Log(GS::UniString::Printf("=== CreateDimensionsBetweenObjects END: создано %d размеров ===", createdCount));
			return true;
		} else {
			Log(GS::UniString::Printf("=== CreateDimensionsBetweenObjects END: создано %d размеров (ошибка: %d) ===", createdCount, (int)err));
			return false;
		}
	}

	// ============================================================================
	// Проставить размеры от объектов до выбранной точки
	// ============================================================================
	bool CreateDimensionsToPoint()
	{
		Log("=== CreateDimensionsToPoint START ===");

		// 1) Получаем выделенные элементы
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		if (ACAPI_Selection_Get(&selInfo, &selNeigs, false) != NoError) {
			Log("ERROR: не удалось получить выделение");
			return false;
		}

		if (selNeigs.IsEmpty()) {
			Log("ERROR: ничего не выделено");
			return false;
		}

		Log(GS::UniString::Printf("Выделено элементов: %d", (int)selNeigs.GetSize()));

		// 2) Собираем точки привязки всех объектов
		struct ObjectPoint {
			API_Coord coord;
			API_Guid guid;
			GS::UniString typeName;
		};
		std::vector<ObjectPoint> objects;

		for (UIndex i = 0; i < selNeigs.GetSize(); ++i) {
			const API_Neig& neig = selNeigs[i];
			
			// Получаем элемент
			API_Element element = {};
			element.header.guid = neig.guid;
			if (ACAPI_Element_Get(&element) != NoError) {
				Log(GS::UniString::Printf("WARN: не удалось получить элемент %d", (int)i));
				continue;
			}

			// Определяем точку привязки в зависимости от типа элемента
			ObjectPoint obj;
			obj.guid = neig.guid;
			bool hasAnchor = false;

			switch (element.header.type.typeID) {
			case API_ObjectID:
				obj.coord = element.object.pos;
				obj.typeName = "Object";
				hasAnchor = true;
				break;
			case API_ColumnID:
				obj.coord = element.column.origoPos;
				obj.typeName = "Column";
				hasAnchor = true;
				break;
			case API_LampID:
				obj.coord = element.lamp.pos;
				obj.typeName = "Lamp";
				hasAnchor = true;
				break;
			case API_WindowID:
			case API_DoorID:
				// Окна и двери не нужны
				continue;
			default:
				Log(GS::UniString::Printf("WARN: неподдерживаемый тип элемента: %d", (int)element.header.type.typeID));
				continue;
			}

			if (hasAnchor) {
				objects.push_back(obj);
				Log(GS::UniString::Printf("Точка %d: (%.3f, %.3f) - %s", 
					(int)objects.size(), obj.coord.x, obj.coord.y, obj.typeName.ToCStr().Get()));
			}
		}

		if (objects.empty()) {
			Log("ERROR: не найдено подходящих объектов для создания размеров");
			return false;
		}

		Log(GS::UniString::Printf("Собрано объектов: %d", (int)objects.size()));

		// 3) Получаем целевую точку от пользователя
		API_GetPointType gp = {}; 
		CHCopyC("Размеры до точки: укажите целевую точку", gp.prompt);
		GSErrCode err = ACAPI_UserInput_GetPoint(&gp);
		if (err != NoError) {
			Log("ERROR: пользователь отменил выбор точки");
			return false;
		}

		const API_Coord targetPoint = { gp.pos.x, gp.pos.y };
		Log(GS::UniString::Printf("Целевая точка: (%.3f, %.3f)", targetPoint.x, targetPoint.y));

		// 4) Создаем размеры от каждого объекта до целевой точки
		int createdCount = 0;
		err = ACAPI_CallUndoableCommand("Размеры до точки", [&]() -> GSErrCode {
			for (size_t i = 0; i < objects.size(); ++i) {
				const API_Coord& objCoord = objects[i].coord;
				
				// Вычисляем расстояние
				double dx = targetPoint.x - objCoord.x;
				double dy = targetPoint.y - objCoord.y;
				double distance = sqrt(dx * dx + dy * dy);

				// Создаем размер от объекта до целевой точки
				if (CreateDimensionBetweenPoints(objCoord, targetPoint)) {
					createdCount++;
					Log(GS::UniString::Printf("Размер %d: %.3fм от объекта %d до точки", 
						createdCount, distance, (int)i+1));
				} else {
					Log(GS::UniString::Printf("ERROR: не удалось создать размер от объекта %d", (int)i+1));
				}
			}
			return NoError;
		});

		if (err == NoError && createdCount > 0) {
			Log(GS::UniString::Printf("=== CreateDimensionsToPoint END: создано %d размеров ===", createdCount));
			return true;
		} else {
			Log(GS::UniString::Printf("=== CreateDimensionsToPoint END: создано %d размеров (ошибка: %d) ===", createdCount, (int)err));
			return false;
		}
	}

} // namespace MarkupHelper

