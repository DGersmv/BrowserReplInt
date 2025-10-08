#include "BuildHelper.hpp"
#include "ACAPinc.h"
#include "BrowserRepl.hpp"
#include <cmath>

namespace BuildHelper {

	// ---------- Stored selections ----------
	static API_Guid g_slabCurveGuid = APINULLGuid;
	static API_Guid g_shellCurveGuid = APINULLGuid;
	static API_Guid g_shellMeshGuid = APINULLGuid;

	static inline void Log(const GS::UniString& msg)
	{
		if (BrowserRepl::HasInstance())
			BrowserRepl::GetInstance().LogToBrowser("[Build] " + msg);
		ACAPI_WriteReport("%s", false, msg.ToCStr().Get());
	}

	static inline bool GuidIsValid(const API_Guid& g) { return !(g == APINULLGuid); }

	// Allowed curve types
	static bool IsCurveType(API_ElemTypeID t)
	{
		return t == API_LineID || t == API_PolyLineID || t == API_ArcID || t == API_SplineID;
	}

	static bool IsMeshType(API_ElemTypeID t) { return t == API_MeshID; }

	// ASCII-only type names (avoid mojibake)
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

	// Pick first selected element satisfying predicate
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

	// ============== shell stubs ==============
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
		API_Element h = {};
		h.header.guid = g;
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
		API_Element h = {};
		h.header.guid = g;
		if (ACAPI_Element_GetHeader(&h.header) != NoError) {
			Log("SetMeshForShell: failed to get mesh header.");
			return false;
		}
		g_shellMeshGuid = g;
		Log("Shell mesh set: " + TypeNameOf(h.header.type.typeID));
		return true;
	}

	// ============== slab helpers ==============

	// Create a slab from a convex polygon given by 'contour' (no holes).
	// Returns NoError or an error code from ACAPI_Element_Create.
	static GSErrCode CreateSlabFromContour(const GS::Array<API_Coord>& contour)
	{
		if (contour.GetSize() < 3)
			return APIERR_GENERAL;

		API_Element slab = {};
		slab.header.type = API_SlabID;
		ACAPI_Element_GetDefaults(&slab, nullptr);

		API_ElementMemo memo = {};
		BNZeroMemory(&memo, sizeof(API_ElementMemo));

		const Int32 nUnique = (Int32)contour.GetSize();
		const Int32 nCoords = nUnique + 1; // + closing point

		// coords: allocate (nCoords + 1) due to 1-based indexing
		memo.coords = reinterpret_cast<API_Coord**>(
			BMAllocateHandle((nCoords + 1) * (GSSize)sizeof(API_Coord), ALLOCATE_CLEAR, 0)
			);
		if (memo.coords == nullptr) {
			Log("Memory allocation failed (coords).");
			return APIERR_MEMFULL;
		}

		for (Int32 i = 0; i < nUnique; ++i)
			(*memo.coords)[i + 1] = contour[i];
		(*memo.coords)[nCoords] = (*memo.coords)[1]; // close

		// pends: one outer subpoly → [0]=0, [1]=nCoords
		memo.pends = reinterpret_cast<Int32**>(
			BMAllocateHandle(2 * (GSSize)sizeof(Int32), ALLOCATE_CLEAR, 0)
			);
		if (memo.pends == nullptr) {
			ACAPI_DisposeElemMemoHdls(&memo);
			Log("Memory allocation failed (pends).");
			return APIERR_MEMFULL;
		}
		(*memo.pends)[0] = 0;
		(*memo.pends)[1] = nCoords;

		slab.slab.poly.nCoords = nCoords;
		slab.slab.poly.nSubPolys = 1;
		slab.slab.poly.nArcs = 0;
		memo.parcs = nullptr;

		const GSErrCode err = ACAPI_Element_Create(&slab, &memo);
		ACAPI_DisposeElemMemoHdls(&memo);
		return err;
	}

	// Build a simple rectangle slab along a segment (p0 -> p1) with given width.
	static GSErrCode CreateRectSlabAlongSegment(const API_Coord& p0, const API_Coord& p1, double width)
	{
		double dx = p1.x - p0.x, dy = p1.y - p0.y;
		const double len = std::sqrt(dx * dx + dy * dy);
		if (len < 1e-9)
			return APIERR_GENERAL;

		dx /= len; dy /= len; // unit tangent
		const double nx = -dy * (width * 0.5);
		const double ny = dx * (width * 0.5);

		GS::Array<API_Coord> contour;
		contour.Push({ p0.x + nx, p0.y + ny });
		contour.Push({ p1.x + nx, p1.y + ny });
		contour.Push({ p1.x - nx, p1.y - ny });
		contour.Push({ p0.x - nx, p0.y - ny });

		return CreateSlabFromContour(contour);
	}

	// ============== public: set curve for slab ==============
	bool SetCurveForSlab()
	{
		API_Guid g;
		if (!PickSingleSelected(g, IsCurveType)) {
			Log("SetCurveForSlab: select a Line/Polyline/Arc/Spline first.");
			return false;
		}

		API_Element h = {};
		h.header.guid = g;
		if (ACAPI_Element_GetHeader(&h.header) != NoError) {
			Log("SetCurveForSlab: failed to get element header.");
			return false;
		}
		g_slabCurveGuid = g;
		Log("Slab curve set: " + TypeNameOf(h.header.type.typeID));
		return true;
	}

	// ============== public: create slab along curve ==============
	bool CreateSlabAlongCurve(double width)
	{
		// Default so the tool "just works"
		if (width <= 0.0) {
			width = 1.0; // meters (internal)
			Log("Width <= 0. Using default width = 1.0");
		}

		// Prefer saved curve; otherwise current selection
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

		API_Element curve = {};
		curve.header.guid = curveGuid;
		if (ACAPI_Element_Get(&curve) != NoError) {
			Log("Failed to get selected element.");
			return false;
		}

		const API_ElemTypeID tid = curve.header.type.typeID;
		if (!IsCurveType(tid)) {
			Log("Selected element is not a curve (Line/Polyline/Arc/Spline).");
			return false;
		}

		// 1) Build axis points
		GS::Array<API_Coord> pts;

		if (tid == API_LineID) {
			pts.Push(curve.line.begC);
			pts.Push(curve.line.endC);

		}
		else if (tid == API_PolyLineID) {
			API_ElementMemo pm = {};
			if (ACAPI_Element_GetMemo(curve.header.guid, &pm, APIMemoMask_Polygon) == NoError && pm.coords != nullptr) {
				const Int32 n = (Int32)(BMGetHandleSize((GSHandle)pm.coords) / sizeof(API_Coord)) - 1;
				for (Int32 i = 1; i <= n; ++i)
					pts.Push((*pm.coords)[i]);
			}
			ACAPI_DisposeElemMemoHdls(&pm);

		}
		else if (tid == API_ArcID) {
			const API_ArcType& a = curve.arc;
			const double a0 = a.begAng;
			const double a1 = a.endAng;
			const int segs = 20;
			const double dA = (a1 - a0) / segs;
			for (int i = 0; i <= segs; ++i) {
				const double ang = a0 + i * dA;
				pts.Push({ a.origC.x + a.r * std::cos(ang), a.origC.y + a.r * std::sin(ang) });
			}

		}
		else if (tid == API_SplineID) {
			// This SDK has no APIMemoMask_Spline — use All and check coords
			API_ElementMemo pm = {};
			if (ACAPI_Element_GetMemo(curve.header.guid, &pm, APIMemoMask_All) == NoError && pm.coords != nullptr) {
				const Int32 n = (Int32)(BMGetHandleSize((GSHandle)pm.coords) / sizeof(API_Coord)) - 1;
				for (Int32 i = 1; i <= n; ++i)
					pts.Push((*pm.coords)[i]);
			}
			ACAPI_DisposeElemMemoHdls(&pm);
		}

		// Remove consecutive duplicates (avoid zero-length edges)
		auto almostEq = [](const API_Coord& a, const API_Coord& b) {
			const double eps = 1e-9;
			return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
			};
		if (pts.GetSize() >= 2) {
			GS::Array<API_Coord> clean;
			clean.Push(pts[0]);
			for (UIndex i = 1; i < pts.GetSize(); ++i)
				if (!almostEq(pts[i], pts[i - 1]))
					clean.Push(pts[i]);
			pts = clean;
		}

		if (pts.GetSize() < 2) {
			Log("Curve has too few points.");
			return false;
		}

		// 2) Try full ribbon along all segments (simple offset per segment)
		GS::Array<API_Coord> left, right;
		for (UIndex i = 0; i + 1 < pts.GetSize(); ++i) {
			const double dx = pts[i + 1].x - pts[i].x;
			const double dy = pts[i + 1].y - pts[i].y;
			const double len = std::sqrt(dx * dx + dy * dy);
			if (len < 1e-9) continue;

			const double nx = -dy / len;
			const double ny = dx / len;

			left.Push({ pts[i].x + nx * (width * 0.5), pts[i].y + ny * (width * 0.5) });
			right.Push({ pts[i].x - nx * (width * 0.5), pts[i].y - ny * (width * 0.5) });

			if (i + 1 == pts.GetSize() - 1) {
				left.Push({ pts[i + 1].x + nx * (width * 0.5), pts[i + 1].y + ny * (width * 0.5) });
				right.Push({ pts[i + 1].x - nx * (width * 0.5), pts[i + 1].y - ny * (width * 0.5) });
			}
		}

		GS::Array<API_Coord> contour;
		for (const auto& c : left) contour.Push(c);
		for (Int32 i = (Int32)right.GetSize() - 1; i >= 0; --i) contour.Push(right[i]);

		GSErrCode err = APIERR_GENERAL;
		if (contour.GetSize() >= 3) {
			err = CreateSlabFromContour(contour);
		}

		// 3) Fallback: if full ribbon fails (self-intersections, etc.), make a simple rectangle along the first segment
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
