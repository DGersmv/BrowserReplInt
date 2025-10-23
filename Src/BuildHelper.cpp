#include "BuildHelper.hpp"
#include "ACAPinc.h"
#include "BrowserRepl.hpp"

#include <cmath>
#include <algorithm>   // std::min/max

namespace BuildHelper {

	// ---------- Stored selections ----------
	static API_Guid g_slabCurveGuid = APINULLGuid;
	static API_Guid g_shellCurveGuid = APINULLGuid;
	static API_Guid g_shellMeshGuid = APINULLGuid;

	// ---------- Utils ----------
	static inline void Log(const GS::UniString& msg)
	{
		// if (BrowserRepl::HasInstance())
		//	BrowserRepl::GetInstance().LogToBrowser("[Build] " + msg);
		// ACAPI_WriteReport("%s", false, msg.ToCStr().Get());
	}

	static inline bool GuidIsValid(const API_Guid& g) { return !(g == APINULLGuid); }

	static bool IsCurveType(API_ElemTypeID t)
	{
		return t == API_LineID || t == API_PolyLineID || t == API_ArcID || t == API_SplineID;
	}
	static bool IsMeshType(API_ElemTypeID t) { return t == API_MeshID; }

	static GS::UniString TypeNameOf(API_ElemTypeID t)
	{
		switch (t) {
		case API_LineID:     return "Line";
		case API_PolyLineID: return "Polyline";
		case API_ArcID:      return "Arc";
		case API_SplineID:   return "Spline";
		case API_MeshID:     return "Mesh";
		default:             return "Element";
		}
	}

	// Выбрать первый подходящий элемент в текущем выделении
	static bool PickSingleSelected(API_Guid& outGuid, bool (*predicate)(API_ElemTypeID))
	{
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> neigs;
		ACAPI_Selection_Get(&selInfo, &neigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		outGuid = APINULLGuid;
		for (const auto& n : neigs) {
			API_Element h = {};
			h.header.guid = n.guid;
			if (ACAPI_Element_GetHeader(&h.header) != NoError)
				continue;
			if (predicate(h.header.type.typeID)) {
				outGuid = h.header.guid;
				return true;
			}
		}
		return false;
	}

	// ------------------------------------------------------------
	// Shell (пока заглушки)
	// ------------------------------------------------------------
	bool CreateShellAlongCurve(double /*width*/)
	{
		Log("CreateShellAlongCurve: not implemented yet.");
		return false;
	}

	bool SetCurveForShell()
	{
		API_Guid g;
		if (!PickSingleSelected(g, IsCurveType)) {
			Log("SetCurveForShell: select a Line/Polyline/Arc/Spline first.");
			return false;
		}
		API_Element h = {}; h.header.guid = g;
		if (ACAPI_Element_GetHeader(&h.header) != NoError) {
			Log("SetCurveForShell: failed to get element header.");
			return false;
		}
		g_shellCurveGuid = g;
		Log("Shell curve set: " + TypeNameOf(h.header.type.typeID));
		return true;
	}

	bool SetMeshForShell()
	{
		API_Guid g;
		if (!PickSingleSelected(g, IsMeshType)) {
			Log("SetMeshForShell: select a Mesh (3D surface) first.");
			return false;
		}
		API_Element h = {}; h.header.guid = g;
		if (ACAPI_Element_GetHeader(&h.header) != NoError) {
			Log("SetMeshForShell: failed to get mesh header.");
			return false;
		}
		g_shellMeshGuid = g;
		Log("Shell mesh set: " + TypeNameOf(h.header.type.typeID));
		return true;
	}

	// ------------------------------------------------------------
	// Вспомогательное: создать плиту из одного внешнего контура
	// ------------------------------------------------------------
	static GSErrCode CreateSlabFromContour(const GS::Array<API_Coord>& contour)
	{
		if (contour.GetSize() < 3)
			return APIERR_GENERAL;

		API_Element slab = {};
		slab.header.type = API_SlabID;
		GSErrCode e = ACAPI_Element_GetDefaults(&slab, nullptr);
		if (e != NoError) return e;



		API_ElementMemo memo = {};
		BNZeroMemory(&memo, sizeof(API_ElementMemo));

		const Int32 nUnique = (Int32)contour.GetSize();
		const Int32 nCoords = nUnique + 1; // + замыкающая точка

		// coords — 1-based! (документация по API_Polygon / ElementMemo) :contentReference[oaicite:1]{index=1}
		memo.coords = reinterpret_cast<API_Coord**>(BMAllocateHandle((nCoords + 1) * (GSSize)sizeof(API_Coord), ALLOCATE_CLEAR, 0));
		if (memo.coords == nullptr) {
			Log("Memory allocation failed (coords).");
			return APIERR_MEMFULL;
		}
		for (Int32 i = 0; i < nUnique; ++i)
			(*memo.coords)[i + 1] = contour[i];
		(*memo.coords)[nCoords] = (*memo.coords)[1]; // замкнуть

		// Один внешний контур: pends = [0, nCoords] (размер nSubPolys+1 = 2) :contentReference[oaicite:2]{index=2}
		memo.pends = reinterpret_cast<Int32**>(BMAllocateHandle(2 * (GSSize)sizeof(Int32), ALLOCATE_CLEAR, 0));
		if (memo.pends == nullptr) {
			ACAPI_DisposeElemMemoHdls(&memo);
			Log("Memory allocation failed (pends).");
			return APIERR_MEMFULL;
		}
		(*memo.pends)[0] = 0;
		(*memo.pends)[1] = nCoords;
		memo.parcs = nullptr; // без дуг

		slab.slab.poly.nCoords = nCoords;
		slab.slab.poly.nSubPolys = 1;
		slab.slab.poly.nArcs = 0;

		// Создать элемент
		e = ACAPI_Element_Create(&slab, &memo);
		ACAPI_DisposeElemMemoHdls(&memo);
		return e;
	}

	// Простой прямоугольник по первому отрезку (fallback)
	static GSErrCode CreateRectSlabAlongSegment(const API_Coord& p0, const API_Coord& p1, double width)
	{
		double dx = p1.x - p0.x, dy = p1.y - p0.y;
		const double len = std::sqrt(dx * dx + dy * dy);
		if (len < 1e-9)
			return APIERR_GENERAL;

		dx /= len; dy /= len;                 // касательная
		const double offX = -dy * (width * 0.5);
		const double offY = dx * (width * 0.5);

		GS::Array<API_Coord> contour;
		contour.Push({ p0.x + offX, p0.y + offY });
		contour.Push({ p1.x + offX, p1.y + offY });
		contour.Push({ p1.x - offX, p1.y - offY });
		contour.Push({ p0.x - offX, p0.y - offY });

		return CreateSlabFromContour(contour);
	}

	// ------------------------------------------------------------
	// Публичное: выбрать кривую для плиты
	// ------------------------------------------------------------
	bool SetCurveForSlab()
	{
		API_Guid g;
		if (!PickSingleSelected(g, IsCurveType)) {
			Log("SetCurveForSlab: select a Line/Polyline/Arc/Spline first.");
			return false;
		}
		API_Element h = {}; h.header.guid = g;
		if (ACAPI_Element_GetHeader(&h.header) != NoError) {
			Log("SetCurveForSlab: failed to get element header.");
			return false;
		}
		g_slabCurveGuid = g;
		Log("Slab curve set: " + TypeNameOf(h.header.type.typeID));
		return true;
	}

	// ------------------------------------------------------------
	// Публичное: создать плиту вдоль кривой (лента шириной width)
	// ------------------------------------------------------------
	bool CreateSlabAlongCurve(double width)
	{
		// Проекты часто вводят мм из UI — автодетект: если ширина слишком большая → считаем, что мм и делим на 1000.
		bool autoMM = false;
		if (std::fabs(width) > 99.0) { width /= 1000.0; autoMM = true; }

		if (width <= 0.0) {
			width = 1.0; // метры (внутренние ед.)
			Log("Width <= 0. Using default width = 1.0 m");
		}
		if (autoMM) {
			GS::UniString m; m.Printf("Width auto-converted mm to m.");
			Log(m);
		}

		// Берём заранее сохранённую кривую, иначе — первую из текущего выделения
		API_Guid curveGuid = g_slabCurveGuid;
		if (!GuidIsValid(curveGuid)) {
			API_SelectionInfo selInfo = {};
			GS::Array<API_Neig> selNeigs;
			ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
			BMKillHandle((GSHandle*)&selInfo.marquee.coords);

			if (selNeigs.IsEmpty()) {
				Log("No curve selected.");
				return false;
			}
			curveGuid = selNeigs[0].guid;
		}

		API_Element curve = {}; curve.header.guid = curveGuid;
		if (ACAPI_Element_Get(&curve) != NoError) {
			Log("Failed to get selected element.");
			return false;
		}
		const API_ElemTypeID tid = curve.header.type.typeID;
		if (!IsCurveType(tid)) {
			Log("Selected element is not a curve (Line/Polyline/Arc/Spline).");
			return false;
		}

		// ---------- 1) собрать осевую ломаную pts ----------
		GS::Array<API_Coord> pts;

		if (tid == API_LineID) {
			pts.Push(curve.line.begC);
			pts.Push(curve.line.endC);
		}
		else if (tid == API_PolyLineID) {
			API_ElementMemo pm = {};
			if (ACAPI_Element_GetMemo(curve.header.guid, &pm, APIMemoMask_Polygon) == NoError && pm.coords != nullptr) {
				const Int32 n = (Int32)(BMGetHandleSize((GSHandle)pm.coords) / sizeof(API_Coord)) - 1;
				for (Int32 i = 1; i <= n; ++i) pts.Push((*pm.coords)[i]);
			}
			ACAPI_DisposeElemMemoHdls(&pm);
		}
		else if (tid == API_ArcID) {
			const API_ArcType& a = curve.arc;
			const double a0 = a.begAng, a1 = a.endAng;
			const int    segs = 24;
			const double dA = (a1 - a0) / segs;
			for (int i = 0; i <= segs; ++i) {
				const double ang = a0 + i * dA;
				pts.Push({ a.origC.x + a.r * std::cos(ang), a.origC.y + a.r * std::sin(ang) });
			}
		}
		else if (tid == API_SplineID) {
			// Для сплайна координаты тоже приходят через memo.coords (см. примеры в сообществе/доках) :contentReference[oaicite:3]{index=3}
			API_ElementMemo pm = {};
			if (ACAPI_Element_GetMemo(curve.header.guid, &pm, APIMemoMask_All) == NoError && pm.coords != nullptr) {
				const Int32 n = (Int32)(BMGetHandleSize((GSHandle)pm.coords) / sizeof(API_Coord)) - 1;
				for (Int32 i = 1; i <= n; ++i) pts.Push((*pm.coords)[i]);
			}
			ACAPI_DisposeElemMemoHdls(&pm);
		}

		// Почистить повторы подряд, чтобы не было нулевых сегментов
		auto almostEq = [](const API_Coord& a, const API_Coord& b) {
			const double eps = 1e-9;
			return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
			};
		if (pts.GetSize() >= 2) {
			GS::Array<API_Coord> clean; clean.Push(pts[0]);
			for (UIndex i = 1; i < pts.GetSize(); ++i)
				if (!almostEq(pts[i], pts[i - 1])) clean.Push(pts[i]);
			pts = clean;
		}
		if (pts.GetSize() < 2) {
			Log("Curve has too few points.");
			return false;
		}

		// ---------- 2) построить ленту (две офсет-кромки), собрать внешний контур ----------
		GS::Array<API_Coord> left, right;
		for (UIndex i = 0; i + 1 < pts.GetSize(); ++i) {
			const double dx = pts[i + 1].x - pts[i].x;
			const double dy = pts[i + 1].y - pts[i].y;
			const double len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-9) continue;

			const double nx = -dy / len;
			const double ny = dx / len;
			const double off = width * 0.5;

			left.Push({ pts[i].x + nx * off,     pts[i].y + ny * off });
			right.Push({ pts[i].x - nx * off,     pts[i].y - ny * off });

			if (i + 1 == pts.GetSize() - 1) {
				left.Push({ pts[i + 1].x + nx * off, pts[i + 1].y + ny * off });
				right.Push({ pts[i + 1].x - nx * off, pts[i + 1].y - ny * off });
			}
		}

		GS::Array<API_Coord> contour;
		for (const auto& c : left) contour.Push(c);
		for (Int32 i = (Int32)right.GetSize() - 1; i >= 0; --i) contour.Push(right[i]);

		// ---------- 3) создать плиту; если не вышло — fallback прямоугольник ----------
		GSErrCode err = APIERR_GENERAL;
		if (contour.GetSize() >= 3)
			err = CreateSlabFromContour(contour);

		if (err != NoError) {
			if (pts.GetSize() >= 2) {
				err = CreateRectSlabAlongSegment(pts[0], pts[1], width);
				if (err == NoError) {
					Log("Slab created (fallback rectangle on first segment).");
					return true;
				}
			}
			GS::UniString msg; msg.Printf("Slab creation failed (err=%d).", err);
			Log(msg);
			return false;
		}

		Log("Slab created successfully.");
		return true;
	}

} // namespace BuildHelper
