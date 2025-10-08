#include "GroundHelper.hpp"
#include "BrowserRepl.hpp"
#include "ACAPinc.h"
#include "APICommon.h"
#include "APIdefs_Elements.h"

#include <cmath>

// ------------------ Globals ------------------
static API_Guid g_surfaceGuid = APINULLGuid;
static GS::Array<API_Guid> g_objectGuids;

// ------------------ Logging ------------------
static inline void LogToPalette(const GS::UniString& msg)
{
	if (BrowserRepl::HasInstance())
		BrowserRepl::GetInstance().LogToBrowser(msg);

	// ОБЯЗАТЕЛЬНО через "%s"
	ACAPI_WriteReport("%s", false, msg.ToCStr().Get());
}

static inline GS::UniString NumI(long long v) { GS::UniString s; s.Printf("%lld", v); return s; }
static inline GS::UniString NumD(double v, int prec = 3) { GS::UniString s; s.Printf("%.*f", prec, v); return s; }
static inline double Sqr(double x) { return x * x; }

// ------------------ Story helpers ------------------

static bool GetStoryLevelZ(short floorInd, double& outZ)
{
	outZ = 0.0;

	API_StoryInfo si = {};
	const GSErr e = ACAPI_ProjectSetting_GetStorySettings(&si);
	if (e != NoError || si.data == nullptr)
		return (e == NoError); // если нет историй — считаем 0

	const Int32 cnt = (Int32)(BMGetHandleSize((GSHandle)si.data) / sizeof(API_StoryType));
	if (floorInd >= 0 && cnt > 0) {
		// floorInd привязан к диапазону [firstStory..lastStory]
		const Int32 idx = floorInd - si.firstStory;
		if (0 <= idx && idx < cnt) {
			const API_StoryType& st = (*si.data)[idx];
			outZ = st.level;
		}
	}
	BMKillHandle((GSHandle*)&si.data);
	return true;
}

// ------------------ Fetch helpers ------------------

// Всегда тянем Header сначала, затем полный Element
static bool FetchElementByGuid(const API_Guid& guid, API_Element& out)
{
	out = {};
	out.header.guid = guid;

	const GSErr errH = ACAPI_Element_GetHeader(&out.header);
	if (errH != NoError) {
		GS::UniString m; m.Printf("[Ground] GetHeader failed guid=%s err=%d", APIGuidToString(guid).ToCStr().Get(), (int)errH);
		LogToPalette(m);
		return false;
	}

	const GSErr errE = ACAPI_Element_Get(&out);
	if (errE != NoError) {
		GS::UniString m; m.Printf("[Ground] Element_Get failed guid=%s typeID=%d err=%d",
			APIGuidToString(guid).ToCStr().Get(), (int)out.header.type.typeID, (int)errE);
		LogToPalette(m);
		return false;
	}
	return true;
}

// ------------------ Geometry helpers ------------------

// Точный поиск индекса вершины (по XY) в memo.coords[1..n]. Возвращает 0, если не найдено.
static inline Int32 FindCoordIdx(const API_ElementMemo& memo, const API_Coord& c)
{
	if (memo.coords == nullptr) return 0;
	const Int32 n = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) - 1;
	for (Int32 i = 1; i <= n; ++i) {
		const API_Coord v = (*memo.coords)[i];
		if (v.x == c.x && v.y == c.y) return i;
	}
	return 0;
}

// Интерполяция Z по плоскости ABC для XY-точки P
static bool InterpolateZOnPlane(const API_Vector3D& A, const API_Vector3D& B, const API_Vector3D& C,
	const API_Coord& Pxy, double& outZ)
{
	const double denom = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
	if (std::fabs(denom) < 1e-12) return false;

	const double w1 = ((B.y - C.y) * (Pxy.x - C.x) + (C.x - B.x) * (Pxy.y - C.y)) / denom;
	const double w2 = ((C.y - A.y) * (Pxy.x - C.x) + (A.x - C.x) * (Pxy.y - C.y)) / denom;
	const double w3 = 1.0 - w1 - w2;

	outZ = w1 * A.z + w2 * B.z + w3 * C.z;
	return true;
}

// Фоллбэк: по 3 ближайшим вершинам меша. ВОЗВРАЩАЕМ относительный к mesh.level (!) Z.
static bool FallbackRelZFromNearest3(const API_ElementMemo& memo, const API_Coord& Pxy, double& outRelZ)
{
	if (memo.coords == nullptr || memo.meshPolyZ == nullptr) return false;

	const API_Coord* coords = *memo.coords;
	const double* Z = *memo.meshPolyZ;
	const Int32 n = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) - 1;
	if (n < 3) return false;

	Int32 i1 = 0, i2 = 0, i3 = 0; double d1 = 1e300, d2 = 1e300, d3 = 1e300;
	for (Int32 i = 1; i <= n; ++i) {
		const double d = Sqr(Pxy.x - coords[i].x) + Sqr(Pxy.y - coords[i].y);
		if (d < d1) { d3 = d2; i3 = i2; d2 = d1; i2 = i1; d1 = d; i1 = i; }
		else if (d < d2) { d3 = d2; i3 = i2; d2 = d; i2 = i; }
		else if (d < d3) { d3 = d; i3 = i; }
	}
	if (i1 == 0 || i2 == 0 || i3 == 0) return false;

	const API_Vector3D A{ coords[i1].x, coords[i1].y, Z[i1] };
	const API_Vector3D B{ coords[i2].x, coords[i2].y, Z[i2] };
	const API_Vector3D C{ coords[i3].x, coords[i3].y, Z[i3] };

	double z = 0.0;
	if (!InterpolateZOnPlane(A, B, C, Pxy, z)) return false;

	outRelZ = z; // относительный к mesh.level
	return true;
}

// Главный расчёт: абсолютный Z и нормаль на Mesh
static bool GetSurfaceZAndNormal_Internal(const API_Guid& meshGuid, const API_Coord3D& pos3D, double& outAbsZ, API_Vector3D& outNormal)
{
	outAbsZ = 0.0;
	outNormal = { 0,0,1 };

	API_Element mesh{};
	if (!FetchElementByGuid(meshGuid, mesh) || mesh.header.type.typeID != API_MeshID) {
		LogToPalette("[Ground] GetSurfaceZAndNormal: not a Mesh or fetch failed");
		return false;
	}

	// Абсолютный «базовый» уровень Mesh (уровень этажа меша + его own level)
	double meshFloorZ = 0.0;
	GetStoryLevelZ(mesh.header.floorInd, meshFloorZ);
	const double meshBaseAbsZ = meshFloorZ + mesh.mesh.level;

	API_ElementMemo memo{};
	API_Coord** triCoordsHdl = nullptr;
	bool ok = false;

	const GSErr errMemo = ACAPI_Element_GetMemo(mesh.header.guid, &memo, APIMemoMask_Polygon | APIMemoMask_MeshPolyZ);
	if (errMemo != NoError) {
		GS::UniString m; m.Printf("[Ground] GetMemo failed, err=%d", (int)errMemo);
		LogToPalette(m);
		goto cleanup;
	}
	if (memo.coords == nullptr || memo.meshPolyZ == nullptr) {
		LogToPalette("[Ground] coords or meshPolyZ is null");
		goto cleanup;
	}

	const GSErr errTri = ACAPI_Polygon_TriangulatePoly(&memo, &triCoordsHdl);
	if (errTri != NoError || triCoordsHdl == nullptr) {
		GS::UniString m; m.Printf("[Ground] triangulation failed, err=%d", (int)errTri);
		LogToPalette(m);
		goto cleanup;
	}

	{
		const Int32 nTriVerts = (Int32)(BMGetHandleSize((GSHandle)triCoordsHdl) / sizeof(API_Coord));
		if (nTriVerts <= 0 || (nTriVerts % 3) != 0) {
			GS::UniString m; m.Printf("[Ground] bad triangulation size=%d", (int)nTriVerts);
			LogToPalette(m);
			goto cleanup;
		}

		const double* zArr = *memo.meshPolyZ;
		bool containedXY = false;
		bool indexMiss = false;

		for (Int32 t = 0; t < nTriVerts; t += 3) {
			const API_Coord a2 = (*triCoordsHdl)[t + 0];
			const API_Coord b2 = (*triCoordsHdl)[t + 1];
			const API_Coord c2 = (*triCoordsHdl)[t + 2];

			const double denom = (b2.y - c2.y) * (a2.x - c2.x) + (c2.x - b2.x) * (a2.y - c2.y);
			if (std::fabs(denom) < 1e-12) continue;

			const double w1 = ((b2.y - c2.y) * (pos3D.x - c2.x) + (c2.x - b2.x) * (pos3D.y - c2.y)) / denom;
			const double w2 = ((c2.y - a2.y) * (pos3D.x - c2.x) + (a2.x - c2.x) * (pos3D.y - c2.y)) / denom;
			const double w3 = 1.0 - w1 - w2;

			if (w1 < -1e-9 || w2 < -1e-9 || w3 < -1e-9) continue; // вне треугольника
			containedXY = true;

			// Индексы исходных вершин (Steiner? может промахнуться)
			const Int32 ia = FindCoordIdx(memo, a2);
			const Int32 ib = FindCoordIdx(memo, b2);
			const Int32 ic = FindCoordIdx(memo, c2);

			double relZ = 0.0; // Z относительно mesh.level
			if (ia == 0 || ib == 0 || ic == 0) {
				indexMiss = true;
				if (!FallbackRelZFromNearest3(memo, { pos3D.x, pos3D.y }, relZ)) {
					continue;
				}
				// нормаль оставим (0,0,1) при фоллбэке
				outAbsZ = meshBaseAbsZ + relZ;
				ok = true;
				break;
			}

			const double za = zArr[ia];
			const double zb = zArr[ib];
			const double zc = zArr[ic];
			relZ = w1 * za + w2 * zb + w3 * zc;

			// Абсолютный Z вершины под точкой:
			outAbsZ = meshBaseAbsZ + relZ;

			// Нормаль как cross(AB, AC)
			const API_Vector3D A{ a2.x, a2.y, za };
			const API_Vector3D B{ b2.x, b2.y, zb };
			const API_Vector3D C{ c2.x, c2.y, zc };
			API_Vector3D AB{ B.x - A.x, B.y - A.y, B.z - A.z };
			API_Vector3D AC{ C.x - A.x, C.y - A.y, C.z - A.z };
			API_Vector3D n{
				AB.y * AC.z - AB.z * AC.y,
				AB.z * AC.x - AB.x * AC.z,
				AB.x * AC.y - AB.y * AC.x
			};
			const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
			outNormal = (len > 1e-12) ? API_Vector3D{ n.x / len, n.y / len, n.z / len } : API_Vector3D{ 0,0,1 };

			ok = true;
			break;
		}

		if (!ok) {
			GS::UniString msg;
			msg.Printf("[Ground] No triangle under XY=(%.3f, %.3f). %s",
				pos3D.x, pos3D.y,
				(!containedXY ? "Outside mesh footprint." : (indexMiss ? "Index mismatch (Steiner) + fallback failed." : ""))
			);
			LogToPalette(msg);
		}
	}

cleanup:
	if (triCoordsHdl != nullptr) BMKillHandle((GSHandle*)&triCoordsHdl);
	ACAPI_DisposeElemMemoHdls(&memo);
	return ok;
}

// ========================= Public API =========================

bool GroundHelper::SetGroundSurface()
{
	g_surfaceGuid = APINULLGuid;

	API_SelectionInfo selInfo = {};
	GS::Array<API_Neig> selNeigs;
	ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
	BMKillHandle((GSHandle*)&selInfo.marquee.coords);

	for (const API_Neig& n : selNeigs) {
		API_Element el = {}; el.header.guid = n.guid;
		const GSErr err = ACAPI_Element_Get(&el);

		GS::UniString m; m.Printf("[Ground] In SetGroundSurface guid=%s err=%d header.typeID=%d",
			APIGuidToString(n.guid).ToCStr().Get(), (int)err, (int)el.header.type.typeID);
		LogToPalette(m);

		if (err != NoError) continue;
		if (el.header.type.typeID == API_MeshID) {
			g_surfaceGuid = n.guid;
			GS::UniString ok; ok.Printf("[Ground] Mesh selected as surface: %s", APIGuidToString(n.guid).ToCStr().Get());
			LogToPalette(ok);
			break;
		}
	}

	if (g_surfaceGuid == APINULLGuid) {
		LogToPalette("[Ground] No mesh selected as surface");
		return false;
	}
	return true;
}

bool GroundHelper::SetGroundObjects()
{
	g_objectGuids.Clear();

	API_SelectionInfo selInfo = {};
	GS::Array<API_Neig> selNeigs;
	ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
	BMKillHandle((GSHandle*)&selInfo.marquee.coords);

	for (const API_Neig& n : selNeigs) {
		API_Element el = {}; el.header.guid = n.guid;
		const GSErr errEl = ACAPI_Element_Get(&el);

		GS::UniString m; m.Printf("[Ground] Neig guid=%s -> header.typeID=%d err=%d",
			APIGuidToString(n.guid).ToCStr().Get(), (int)el.header.type.typeID, (int)errEl);
		LogToPalette(m);

		if (errEl != NoError) continue;

		const short tid = el.header.type.typeID;
		if ((tid == API_ObjectID || tid == API_LampID || tid == API_ColumnID) && n.guid != g_surfaceGuid) {
			g_objectGuids.Push(n.guid);
			GS::UniString m2; m2.Printf("[Ground] Will land: %s", APIGuidToString(n.guid).ToCStr().Get());
			LogToPalette(m2);
		}
		else if (tid != API_ObjectID && tid != API_LampID && tid != API_ColumnID) {
			LogToPalette("[Ground] Skip: not object/lamp/column");
		}
	}

	{
		GS::UniString m; m.Printf("[Ground] Objects count = %u", (unsigned)g_objectGuids.GetSize());
		LogToPalette(m);
	}
	return !g_objectGuids.IsEmpty();
}

bool GroundHelper::GetGroundZAndNormal(const API_Coord3D& pos3D, double& z, API_Vector3D& normal)
{
	if (g_surfaceGuid == APINULLGuid) {
		LogToPalette("[Ground] surface not set");
		return false;
	}
	return GetSurfaceZAndNormal_Internal(g_surfaceGuid, pos3D, z, normal);
}

bool GroundHelper::ApplyGroundOffset(double offset)
{
	if (g_surfaceGuid == APINULLGuid || g_objectGuids.IsEmpty()) {
		LogToPalette("[Ground] ApplyGroundOffset: no surface or no objects");
		return false;
	}

	GS::UniString st;
	st.Printf("[Ground] Landing %u elements offset=%s",
		(unsigned)g_objectGuids.GetSize(), NumD(offset).ToCStr().Get());
	LogToPalette(st);

	return (ACAPI_CallUndoableCommand("Ground Offset", [&]() -> GSErrCode {

		for (const API_Guid& guid : g_objectGuids) {
			GS::UniString dbg; dbg.Printf("[Ground] Debug: processing guid = %s", APIGuidToString(guid).ToCStr().Get());
			LogToPalette(dbg);

			API_Element element{};
			if (!FetchElementByGuid(guid, element)) {
				LogToPalette("[Ground] FetchElement FAILED, skip");
				continue;
			}

			GS::UniString ti; ti.Printf("[Ground] typeID=%d floorInd=%d", (int)element.header.type.typeID, (int)element.header.floorInd);
			LogToPalette(ti);

			// Абсолютный уровень этажа элемента
			double elemFloorZ = 0.0;
			GetStoryLevelZ(element.header.floorInd, elemFloorZ);

			// Точка опоры по типу (абсолютный Z)
			API_Coord3D pos3D{ 0,0,0 };
			switch (element.header.type.typeID) {
			case API_ObjectID:
				pos3D.x = element.object.pos.x;
				pos3D.y = element.object.pos.y;
				pos3D.z = elemFloorZ + element.object.level; // abs
				break;
			case API_LampID:
				pos3D.x = element.lamp.pos.x;
				pos3D.y = element.lamp.pos.y;
				pos3D.z = elemFloorZ + element.lamp.level; // abs
				break;
			case API_ColumnID:
				pos3D.x = element.column.origoPos.x;
				pos3D.y = element.column.origoPos.y;
				pos3D.z = elemFloorZ + element.column.bottomOffset; // abs
				break;
			default:
				LogToPalette("[Ground] Unsupported type, skip");
				continue;
			}

			{
				GS::UniString p; p.Printf("[Ground] pos3D = (%.3f, %.3f, %.3f)", pos3D.x, pos3D.y, pos3D.z);
				LogToPalette(p);
			}

			// Z поверхности под XY
			double surfaceAbsZ = 0.0;
			API_Vector3D normal{ 0,0,1 };
			const bool got = GetSurfaceZAndNormal_Internal(g_surfaceGuid, pos3D, surfaceAbsZ, normal);
			if (!got) {
				LogToPalette("[Ground] Could not get surface Z -> skip");
				continue;
			}

			const double finalAbsZ = surfaceAbsZ + offset;  // <— offset используем КАК ЕСТЬ
			{
				GS::UniString zmsg; zmsg.Printf("[Ground] baseZ=%.3f finalZ=%.3f", surfaceAbsZ, finalAbsZ);
				LogToPalette(zmsg);
			}

			// Пишем СМЕЩЕНИЕ относительно этажа элемента
			API_Element mask{}; ACAPI_ELEMENT_MASK_CLEAR(mask);
			switch (element.header.type.typeID) {
			case API_ObjectID:
				element.object.level = finalAbsZ - elemFloorZ;
				ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, level);
				break;
			case API_LampID:
				element.lamp.level = finalAbsZ - elemFloorZ;
				ACAPI_ELEMENT_MASK_SET(mask, API_LampType, level);
				break;
			case API_ColumnID:
				element.column.bottomOffset = finalAbsZ - elemFloorZ;
				ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, bottomOffset);
				break;
			}

			const GSErr chg = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
			if (chg == NoError) {
				GS::UniString ok; ok.Printf("[Ground] Updated guid=%s", APIGuidToString(guid).ToCStr().Get());
				LogToPalette(ok);
			}
			else {
				GS::UniString er; er.Printf("[Ground] Change failed err=%d guid=%s", (int)chg, APIGuidToString(guid).ToCStr().Get());
				LogToPalette(er);
			}
		}

		return NoError;
		}) == NoError);
}

bool GroundHelper::DebugOneSelection()
{
	API_SelectionInfo selInfo = {};
	GS::Array<API_Neig> selNeigs;
	ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
	BMKillHandle((GSHandle*)&selInfo.marquee.coords);

	if (selNeigs.IsEmpty()) { LogToPalette("[Debug] No selection"); return false; }

	const API_Neig& n = selNeigs[0];
	API_Element el{};
	if (!FetchElementByGuid(n.guid, el)) {
		LogToPalette("[Debug] FetchElement FAILED");
		return false;
	}

	GS::UniString h; h.Printf("[Debug] guid=%s typeID=%d floorInd=%d",
		APIGuidToString(n.guid).ToCStr().Get(), (int)el.header.type.typeID, (int)el.header.floorInd);
	LogToPalette(h);

	switch (el.header.type.typeID) {
	case API_ObjectID: {
		GS::UniString m; m.Printf("[Debug] Object pos=(%.3f, %.3f), level=%.3f",
			el.object.pos.x, el.object.pos.y, el.object.level);
		LogToPalette(m);
		break;
	}
	case API_LampID: {
		GS::UniString m; m.Printf("[Debug] Lamp pos=(%.3f, %.3f), level=%.3f",
			el.lamp.pos.x, el.lamp.pos.y, el.lamp.level);
		LogToPalette(m);
		break;
	}
	case API_ColumnID: {
		GS::UniString m; m.Printf("[Debug] Column origo=(%.3f, %.3f), bottomOffset=%.3f",
			el.column.origoPos.x, el.column.origoPos.y, el.column.bottomOffset);
		LogToPalette(m);
		break;
	}
	default:
		LogToPalette("[Debug] Other type or empty");
		break;
	}

	return true;
}
