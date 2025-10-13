// GroundHelper.cpp — TIN-landing по Mesh (CDT + edge flips)
//   • Узлы: контур (z = baseZ + meshPolyZ) + level-точки (абсолютный z)
//   • Триангуляция: ear-clipping по внешнему контуру
//   • Вставка level-точек: снап к вершине, вставка на ребро, либо как Штайнер внутрь
//   • Констрейнты: внешний контур всегда; level-линии при наличии
//   • Легализация: constrained Delaunay через edge-flip (глобальный проход до стабилизации)
//   • Семплинг z по барицентрам, нормаль из 3D-плоскости треугольника
//   • Подробные логи

#include "GroundHelper.hpp"
#include "BrowserRepl.hpp"

#include "ACAPinc.h"
#include "APICommon.h"
#include "APIdefs_3D.h"
#include "APIdefs_Elements.h"
#include "APIdefs_Goodies.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <limits>
#include <map>
#include <set>

// ====================== switches ======================
#define ENABLE_PROBE_ADD_POINT 0   // 1 to enable test point injector

// ------------------ Globals ------------------
static API_Guid g_surfaceGuid = APINULLGuid;
static GS::Array<API_Guid> g_objectGuids;

// ------------------ Logging ------------------
static inline void Log(const char* fmt, ...)
{
    va_list vl; va_start(vl, fmt);
    char buf[4096]; vsnprintf(buf, sizeof(buf), fmt, vl);
    va_end(vl);
    GS::UniString s(buf);
    if (BrowserRepl::HasInstance())
        BrowserRepl::GetInstance().LogToBrowser(s);
    ACAPI_WriteReport("%s", false, s.ToCStr().Get());
}

// ================================================================
// Stories
// ================================================================
static bool GetStoryLevelZ(short floorInd, double& outZ)
{
    outZ = 0.0;
    API_StoryInfo si{};
    const GSErr e = ACAPI_ProjectSetting_GetStorySettings(&si);
    if (e != NoError || si.data == nullptr) {
        Log("[GetStory] failed or empty story settings err=%d", (int)e);
        return (e == NoError);
    }
    const Int32 cnt = (Int32)(BMGetHandleSize((GSHandle)si.data) / sizeof(API_StoryType));
    if (floorInd >= 0 && cnt > 0) {
        const Int32 idx = floorInd - si.firstStory;
        if (0 <= idx && idx < cnt) outZ = (*si.data)[idx].level;
    }
    BMKillHandle((GSHandle*)&si.data);
    return true;
}

// ================================================================
// Fetch element header & full element
// ================================================================
static bool FetchElementByGuid(const API_Guid& guid, API_Element& out)
{
    out = {};
    out.header.guid = guid;

    const GSErr errH = ACAPI_Element_GetHeader(&out.header);
    if (errH != NoError) {
        Log("[Fetch] GetHeader failed guid=%s err=%d", APIGuidToString(guid).ToCStr().Get(), (int)errH);
        return false;
    }
    const GSErr errE = ACAPI_Element_Get(&out);
    if (errE != NoError) {
        Log("[Fetch] Element_Get failed guid=%s typeID=%d err=%d",
            APIGuidToString(guid).ToCStr().Get(), (int)out.header.type.typeID, (int)errE);
        return false;
    }
    return true;
}

// ================================================================
// Small math helpers
// ================================================================
static inline void Normalize(API_Vector3D& v)
{
    const double L = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (L > 1e-12) { v.x /= L; v.y /= L; v.z /= L; }
}

static inline double Cross2D(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static inline bool NearlyEq(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

// ================================================================
// Dump ALL 2D mesh points (contours + levels) with Z-layers
// ================================================================
static void LogMesh2DCoords(const API_Guid& meshGuid)
{
    API_Element elem{}; elem.header.guid = meshGuid;
    if (ACAPI_Element_Get(&elem) != NoError) { Log("[Mesh2D] Element_Get failed"); return; }

    API_ElementMemo memo{};
    const GSErrCode em = ACAPI_Element_GetMemo(
        meshGuid, &memo,
        APIMemoMask_Polygon | APIMemoMask_MeshPolyZ | APIMemoMask_MeshLevel
    );
    if (em != NoError) { Log("[Mesh2D] GetMemo failed err=%d", (int)em); return; }

    const Int32 nCoords = elem.mesh.poly.nCoords;                 // includes closing vertex
    API_Coord* coordsH = memo.coords ? *memo.coords : nullptr;    // usually 1-based
    Int32* pEnds = memo.pends ? *memo.pends : nullptr;            // 1-based; pEnds[0]==0
    const Int32 coordsSz = memo.coords ? (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) : 0;

    // Z array along contour nodes:
    double* zH = memo.meshPolyZ ? *memo.meshPolyZ : nullptr;
    const Int32 zCount = zH ? (Int32)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double)) : 0;
    Int32 strideZ = 0;
    if (zH) {
        if (zCount % nCoords == 0)            strideZ = nCoords;
        else if (zCount % (nCoords + 1) == 0) strideZ = nCoords + 1; // 1-based layout
        else                                   strideZ = nCoords;
    }
    const bool coords1 = (coordsSz == nCoords + 1);
    const bool z1 = (strideZ == nCoords + 1);
    Int32 zLayers = (zH && strideZ > 0) ? (zCount / strideZ) : 0;
    if (zH && zLayers <= 0) zLayers = 1;

    // Level points:
    API_MeshLevelCoord* lvl = memo.meshLevelCoords ? *memo.meshLevelCoords : nullptr;
    const Int32 lvlCnt = lvl ? (Int32)(BMGetHandleSize((GSHandle)memo.meshLevelCoords) / sizeof(API_MeshLevelCoord)) : 0;
    Int32* lvlEnds = memo.meshLevelEnds ? *memo.meshLevelEnds : nullptr;
    const Int32 lvlEndsSz = lvlEnds ? (Int32)(BMGetHandleSize((GSHandle)memo.meshLevelEnds) / sizeof(Int32)) : 0;
    const Int32 lvlLines = (lvlEndsSz > 0) ? (lvlEndsSz - 1) : (lvlCnt > 0 ? 1 : 0);

    Log("[Mesh2D] poly: nCoords=%d, nSubPolys=%d, coordsSz=%d", (int)nCoords, (int)elem.mesh.poly.nSubPolys, (int)coordsSz);
    Log("[Mesh2D] Z arrays: zCount=%d, strideZ=%d -> zLayers=%d", (int)zCount, (int)strideZ, (int)zLayers);
    Log("[Mesh2D] level points: count=%d, levelEndsSz=%d, lines=%d", (int)lvlCnt, (int)lvlEndsSz, (int)lvlLines);

    if (coordsH != nullptr) {
        if (pEnds != nullptr) {
            Int32 prevEnd = pEnds[0]; // 0
            for (Int32 sp = 1; sp <= elem.mesh.poly.nSubPolys; ++sp) {
                const Int32 beg = prevEnd + 1;
                const Int32 end = pEnds[sp];
                Log("[Mesh2D] Subpoly %d: indices %d..%d", (int)sp, (int)beg, (int)end);
                for (Int32 i = beg; i <= end; ++i) {
                    const API_Coord& c = coordsH[coords1 ? i : (i - 1)];
                    GS::UniString line = GS::UniString::Printf("[Mesh2D]   #%d: (%.6f, %.6f)", (int)i, c.x, c.y);
                    if (zH && zLayers > 0) {
                        const Int32 zi = z1 ? i : (i - 1);
                        for (Int32 L = 0; L < zLayers; ++L) {
                            const Int32 idx = L * strideZ + zi;
                            if (0 <= idx && idx < zCount)
                                line += GS::UniString::Printf("  Z[%d]=%.6f", (int)L, zH[idx]);
                        }
                    }
                    Log("%s", line.ToCStr().Get());
                }
                prevEnd = end;
            }
        }
        else {
            Log("[Mesh2D] pends == nullptr (single contour fallback)");
            for (Int32 i = 1; i <= nCoords; ++i) {
                const API_Coord& c = coordsH[coords1 ? i : (i - 1)];
                Log("[Mesh2D]   #%d: (%.6f, %.6f)", (int)i, c.x, c.y);
            }
        }
    }
    else {
        Log("[Mesh2D] coords == nullptr");
    }

    // Level points dump
    if (lvl && lvlCnt > 0) {
        if (lvlEnds && lvlEndsSz >= 2) {
            Int32 prev = (*memo.meshLevelEnds)[0];
            for (Int32 ln = 1; ln < lvlEndsSz; ++ln) {
                const Int32 beg = prev;
                const Int32 end = (*memo.meshLevelEnds)[ln];
                Log("[Mesh2D] Level line %d: indices %d..%d (0-based, end exclusive)", (int)ln, (int)beg, (int)(end - 1));
                for (Int32 i = beg; i < end; ++i) {
                    const API_MeshLevelCoord& p = lvl[i];
                    Log("[Mesh2D]   L#%d: (%.6f, %.6f)  Z=%.6f  vertexID=%d",
                        (int)(i + 1), p.c.x, p.c.y, p.c.z, (int)p.vertexID);
                }
                prev = end;
            }
        }
        else {
            Log("[Mesh2D] Level points (flat): count=%d (0-based)", (int)lvlCnt);
            for (Int32 i = 0; i < lvlCnt; ++i) {
                const API_MeshLevelCoord& p = lvl[i];
                Log("[Mesh2D]   L#%d: (%.6f, %.6f)  Z=%.6f  vertexID=%d",
                    (int)(i + 1), p.c.x, p.c.y, p.c.z, (int)p.vertexID);
            }
        }
    }
    else {
        Log("[Mesh2D] No level points");
    }

    ACAPI_DisposeElemMemoHdls(&memo);
}

// ================================================================
// Probe: add a single level point at (x,y,z)
// ================================================================
static bool Probe_AddLevelPointAt(const API_Guid& meshGuid, double x, double y, double z)
{
    if (meshGuid == APINULLGuid) { Log("[Probe] No mesh to inject test point into."); return false; }

    API_Element mesh{}; mesh.header.guid = meshGuid;
    if (ACAPI_Element_Get(&mesh) != NoError) { Log("[Probe] Element_Get failed."); return false; }

    API_ElementMemo oldMemo{};
    if (ACAPI_Element_GetMemo(meshGuid, &oldMemo, APIMemoMask_MeshLevel) != NoError) {
        Log("[Probe] GetMemo(MeshLevel) failed."); return false;
    }

    API_MeshLevelCoord* srcLvl = (oldMemo.meshLevelCoords ? *oldMemo.meshLevelCoords : nullptr);
    Int32* srcEnds = (oldMemo.meshLevelEnds ? *oldMemo.meshLevelEnds : nullptr);

    const Int32 oldCnt = srcLvl ? (Int32)(BMGetHandleSize((GSHandle)oldMemo.meshLevelCoords) / sizeof(API_MeshLevelCoord)) : 0;
    const Int32 oldEndsCnt = srcEnds ? (Int32)(BMGetHandleSize((GSHandle)oldMemo.meshLevelEnds) / sizeof(Int32)) : 0;

    const Int32 newCnt = oldCnt + 1 + 1; // keep [0] unused convention
    GSHandle newLvlH = BMAllocateHandle(newCnt * sizeof(API_MeshLevelCoord), ALLOCATE_CLEAR, 0);
    if (newLvlH == nullptr) { ACAPI_DisposeElemMemoHdls(&oldMemo); Log("[Probe] Allocation failed for level coords."); return false; }
    API_MeshLevelCoord* newLvl = (API_MeshLevelCoord*)*newLvlH;

    if (oldCnt > 0 && srcLvl != nullptr) {
        std::memcpy(newLvl, srcLvl, oldCnt * sizeof(API_MeshLevelCoord));
    }

    const Int32 newIdx = oldCnt + 1;
    newLvl[newIdx].c.x = x;
    newLvl[newIdx].c.y = y;
    newLvl[newIdx].c.z = z;
    newLvl[newIdx].vertexID = 0;

    GSHandle newEndsH = nullptr;
    if (oldEndsCnt > 0 && srcEnds != nullptr) {
        newEndsH = BMAllocateHandle(oldEndsCnt * sizeof(Int32), ALLOCATE_CLEAR, 0);
        if (newEndsH == nullptr) {
            BMKillHandle(&newLvlH);
            ACAPI_DisposeElemMemoHdls(&oldMemo);
            Log("[Probe] Allocation failed for level ends."); return false;
        }
        std::memcpy(*newEndsH, srcEnds, oldEndsCnt * sizeof(Int32));
        Int32* e = (Int32*)*newEndsH;
        e[oldEndsCnt - 1] = newIdx; // include the new point
    }
    else {
        newEndsH = BMAllocateHandle(2 * sizeof(Int32), ALLOCATE_CLEAR, 0);
        if (newEndsH == nullptr) {
            BMKillHandle(&newLvlH);
            ACAPI_DisposeElemMemoHdls(&oldMemo);
            Log("[Probe] Allocation failed for level ends (fresh)."); return false;
        }
        Int32* e = (Int32*)*newEndsH; e[0] = 0; e[1] = newIdx;
    }

    API_ElementMemo newMemo{};
    newMemo.meshLevelCoords = (API_MeshLevelCoord**)newLvlH;
    newMemo.meshLevelEnds = (Int32**)newEndsH;

    const GSErr chg = ACAPI_Element_Change(&mesh, nullptr, &newMemo, APIMemoMask_MeshLevel, true);

    ACAPI_DisposeElemMemoHdls(&newMemo);
    ACAPI_DisposeElemMemoHdls(&oldMemo);

    if (chg == NoError) {
        Log("[Probe] Injected a test level point at (%.6f, %.6f, %.6f).", x, y, z);
        Log("[Probe] Dumping mesh 2D/level data after injection:");
        LogMesh2DCoords(meshGuid);
        return true;
    }
    else {
        Log("[Probe] Failed to inject test level point, err=%d.", (int)chg);
        return false;
    }
}

// ================================================================
// TIN structures & helpers
// ================================================================
struct TINNode { double x, y, z; };
struct TINTri { int a, b, c; }; // indices into nodes

static inline double TriArea2D(const TINNode& A, const TINNode& B, const TINNode& C) {
    return 0.5 * Cross2D(A.x, A.y, B.x, B.y, C.x, C.y);
}
static inline bool IsCCW_Poly(const std::vector<TINNode>& poly) {
    double A = 0.0;
    for (size_t i = 0, n = poly.size(); i < n; ++i) {
        const TINNode& p = poly[i];
        const TINNode& q = poly[(i + 1) % n];
        A += p.x * q.y - p.y * q.x;
    }
    return A > 0.0;
}
static inline bool PointInTriStrict(const TINNode& P, const TINNode& A, const TINNode& B, const TINNode& C) {
    const double c1 = Cross2D(A.x, A.y, B.x, B.y, P.x, P.y);
    const double c2 = Cross2D(B.x, B.y, C.x, C.y, P.x, P.y);
    const double c3 = Cross2D(C.x, C.y, A.x, A.y, P.x, P.y);
    return ((c1 > 0.0) && (c2 > 0.0) && (c3 > 0.0)) || ((c1 < 0.0) && (c2 < 0.0) && (c3 < 0.0));
}
static inline bool PointInTriXY(const TINNode& P, const TINNode& A, const TINNode& B, const TINNode& C) {
    const double c1 = Cross2D(A.x, A.y, B.x, B.y, P.x, P.y);
    const double c2 = Cross2D(B.x, B.y, C.x, C.y, P.x, P.y);
    const double c3 = Cross2D(C.x, C.y, A.x, A.y, P.x, P.y);
    const bool hasNeg = (c1 < 0.0) || (c2 < 0.0) || (c3 < 0.0);
    const bool hasPos = (c1 > 0.0) || (c2 > 0.0) || (c3 > 0.0);
    return !(hasNeg && hasPos);
}
static inline void BaryXY(const TINNode& P, const TINNode& A, const TINNode& B, const TINNode& C,
    double& wA, double& wB, double& wC) {
    const double areaABC = TriArea2D(A, B, C);
    if (std::fabs(areaABC) < 1e-14) { wA = wB = wC = 0.0; return; }
    const double areaPBC = TriArea2D(P, B, C);
    const double areaPCA = TriArea2D(P, C, A);
    wA = areaPBC / areaABC; wB = areaPCA / areaABC; wC = 1.0 - wA - wB;
}
static inline API_Vector3D TriNormal3D(const TINNode& A, const TINNode& B, const TINNode& C) {
    const double ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
    const double vx = C.x - A.x, vy = C.y - A.y, vz = C.z - A.z;
    API_Vector3D n{ uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
    Normalize(n);
    if (n.z < 0.0) { n.x = -n.x; n.y = -n.y; n.z = -n.z; }
    return n;
}

// --- snap helpers ---
static int FindExistingNodeByXY(const std::vector<TINNode>& nodes, double x, double y, double eps = 1e-6)
{
    for (int i = 0; i < (int)nodes.size(); ++i)
        if (NearlyEq(nodes[i].x, x, eps) && NearlyEq(nodes[i].y, y, eps)) return i;
    return -1;
}

static bool PointOnSegmentXY(const TINNode& A, const TINNode& B, const TINNode& P, double eps = 1e-6)
{
    const double vx = B.x - A.x, vy = B.y - A.y;
    const double wx = P.x - A.x, wy = P.y - A.y;
    const double cross = std::fabs(vx * wy - vy * wx);
    const double len = std::hypot(vx, vy);
    if (len < 1e-12) return false;
    if (cross / len > eps) return false; // не на линии

    const double dot = (wx * (B.x - A.x) + wy * (B.y - A.y)) / (len * len);
    return dot >= -1e-9 && dot <= 1.0 + 1e-9;
}

static std::vector<int> FindTrianglesWithEdge(const std::vector<TINTri>& tris, int u, int v)
{
    std::vector<int> res;
    for (int i = 0; i < (int)tris.size(); ++i) {
        const TINTri& t = tris[i];
        if ((t.a == u && t.b == v) || (t.a == v && t.b == u) ||
            (t.b == u && t.c == v) || (t.b == v && t.c == u) ||
            (t.c == u && t.a == v) || (t.c == v && t.a == u))
            res.push_back(i);
    }
    return res;
}

// ================================================================
// Edges / constraints / CDT helpers
// ================================================================
struct Edge { int u, v; };
static inline Edge MkE(int a, int b) { if (a > b) std::swap(a, b); return { a, b }; }
struct EdgeLess {
    bool operator()(const Edge& a, const Edge& b) const {
        return a.u < b.u || (a.u == b.u && a.v < b.v);
    }
};
using EdgeSet = std::set<Edge, EdgeLess>;

static inline bool IsConstrained(const EdgeSet& cs, int a, int b) { return cs.count(MkE(a, b)) > 0; }

static inline bool InCircleCCW(const TINNode& A, const TINNode& B, const TINNode& C, const TINNode& P)
{
    // Для CCW(ABC): det > 0 ⇔ P внутри окружности ABC
    const double ax = A.x - P.x, ay = A.y - P.y;
    const double bx = B.x - P.x, by = B.y - P.y;
    const double cx = C.x - P.x, cy = C.y - P.y;
    double det =
        (ax * ax + ay * ay) * (bx * cy - by * cx) -
        (bx * bx + by * by) * (ax * cy - ay * cx) +
        (cx * cx + cy * cy) * (ax * by - ay * bx);
    // нормализуем знак под CCW
    const double areaABC = Cross2D(A.x, A.y, B.x, B.y, C.x, C.y);
    if (areaABC < 0.0) det = -det;
    return det > 0.0;
}

static inline int Opposite(const TINTri& t, int u, int v)
{
    if (t.a != u && t.a != v) return t.a;
    if (t.b != u && t.b != v) return t.b;
    return t.c;
}

static inline void MakeCCW(const std::vector<TINNode>& nodes, TINTri& t)
{
    if (TriArea2D(nodes[t.a], nodes[t.b], nodes[t.c]) < 0.0) std::swap(t.b, t.c);
}

// ================================================================
// Build contour nodes (внешний контур), z = baseZ + meshPolyZ
// ================================================================
struct MeshPolyData {
    std::vector<TINNode> contour;  // without closing duplicated node
    bool ok = false;
};

static MeshPolyData BuildContourNodes(const API_Element& elem, const API_ElementMemo& memo, double baseZ) {
    MeshPolyData out{};
    if (memo.coords == nullptr || memo.meshPolyZ == nullptr || elem.mesh.poly.nCoords < 3)
        return out;

    API_Coord* coords = *memo.coords;
    const Int32 nCoords = elem.mesh.poly.nCoords; // includes closing
    const Int32 coordsSz = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord));
    const bool coords1 = (coordsSz == nCoords + 1);

    double* zH = *memo.meshPolyZ;
    const Int32 zCount = (Int32)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double));
    Int32 strideZ = 0;
    if (zCount % nCoords == 0) strideZ = nCoords;
    else if (zCount % (nCoords + 1) == 0) strideZ = nCoords + 1;
    else                                  strideZ = nCoords;
    const bool z1 = (strideZ == nCoords + 1);

    out.contour.reserve((size_t)(nCoords - 1));
    for (Int32 i = 1; i <= nCoords - 1; ++i) {
        const API_Coord& c = coords[coords1 ? i : (i - 1)];
        const Int32 zi = z1 ? i : (i - 1);
        const double z = baseZ + zH[zi];
        out.contour.push_back({ c.x, c.y, z });
    }
    out.ok = true;
    return out;
}

// ================================================================
// Ear-clipping triangulation on outer contour
// ================================================================
static std::vector<TINTri> TriangulateEarClipping(const std::vector<TINNode>& poly) {
    std::vector<TINTri> tris;
    const size_t n = poly.size();
    if (n < 3) return tris;

    std::vector<int> idx(n);
    for (size_t i = 0;i < n;++i) idx[i] = (int)i;

    const bool ccw = IsCCW_Poly(poly);
    auto isConvex = [&](int i0, int i1, int i2) {
        const TINNode& A = poly[idx[i0]];
        const TINNode& B = poly[idx[i1]];
        const TINNode& C = poly[idx[i2]];
        const double cross = Cross2D(A.x, A.y, B.x, B.y, C.x, C.y);
        return ccw ? (cross > 0.0) : (cross < 0.0);
        };

    size_t guard = 0;
    while (idx.size() > 3 && guard < n * n) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            const int i0 = (int)((i + idx.size() - 1) % idx.size());
            const int i1 = (int)i;
            const int i2 = (int)((i + 1) % idx.size());
            if (!isConvex(i0, i1, i2)) continue;

            const TINNode& A = poly[idx[i0]];
            const TINNode& B = poly[idx[i1]];
            const TINNode& C = poly[idx[i2]];
            bool empty = true;
            for (size_t k = 0; k < idx.size(); ++k) {
                if (k == (size_t)i0 || k == (size_t)i1 || k == (size_t)i2) continue;
                if (PointInTriStrict(poly[idx[k]], A, B, C)) { empty = false; break; }
            }
            if (!empty) continue;

            TINTri t{ idx[i0], idx[i1], idx[i2] };
            if (!ccw) std::swap(t.b, t.c);
            tris.push_back(t);
            idx.erase(idx.begin() + i1);
            clipped = true;
            break;
        }
        if (!clipped) break;
        ++guard;
    }
    if (idx.size() == 3) {
        TINTri t{ idx[0], idx[1], idx[2] };
        if (TriArea2D(poly[t.a], poly[t.b], poly[t.c]) < 0.0) std::swap(t.b, t.c);
        tris.push_back(t);
    }
    return tris;
}

// ================================================================
// Insert Steiner point (split a triangle into 3) / Split on edge
// ================================================================
static int FindTriContaining(const std::vector<TINNode>& nodes,
    const std::vector<TINTri>& tris,
    const TINNode& P)
{
    for (int ti = 0; ti < (int)tris.size(); ++ti) {
        const TINTri& t = tris[ti];
        if (PointInTriXY(P, nodes[t.a], nodes[t.b], nodes[t.c]))
            return ti;
    }
    return -1;
}

static void SplitTriByPoint(std::vector<TINNode>& nodes, std::vector<TINTri>& tris,
    int triIndex, int pIdx, int& t0, int& t1, int& t2)
{
    const TINTri t = tris[triIndex];
    TINTri A{ t.a, t.b, pIdx };
    TINTri B{ t.b, t.c, pIdx };
    TINTri C{ t.c, t.a, pIdx };
    MakeCCW(nodes, A); MakeCCW(nodes, B); MakeCCW(nodes, C);
    tris[triIndex] = A; t0 = triIndex;
    tris.push_back(B);  t1 = (int)tris.size() - 1;
    tris.push_back(C);  t2 = (int)tris.size() - 1;
}

// разделение треугольника, если точка P лежит на ребре (u,v)
static void SplitTriangleOnEdge(std::vector<TINNode>& nodes, std::vector<TINTri>& tris,
    int triIdx, int u, int v, int pIdx)
{
    TINTri t = tris[triIdx];
    int w = -1;
    if ((t.a == u && t.b == v) || (t.a == v && t.b == u)) w = t.c;
    else if ((t.b == u && t.c == v) || (t.b == v && t.c == u)) w = t.a;
    else if ((t.c == u && t.a == v) || (t.c == v && t.a == u)) w = t.b;
    else return;

    TINTri t1{ u, pIdx, w }; MakeCCW(nodes, t1);
    TINTri t2{ pIdx, v, w }; MakeCCW(nodes, t2);

    tris[triIdx] = t1;
    tris.push_back(t2);
}

// ================================================================
// (Опц.) «прорезка» констрейнта по отрезку level: вернуть цепочку узлов
//    chain: ia -> ...inserted... -> ib  (включая концы)
// ================================================================
static std::vector<int> CarveConstraintChain(std::vector<TINNode>& nodes, std::vector<TINTri>& tris, int ia, int ib)
{
    struct IP { double t; int idx; };
    const TINNode A = nodes[ia], B = nodes[ib];
    const double segLen = std::hypot(B.x - A.x, B.y - A.y);
    if (segLen < 1e-9) return { ia, ib };

    std::vector<IP> ips;
    bool progressed = true;
    size_t guard = 0;
    while (progressed && guard < 4096) {
        progressed = false; ++guard;

        for (int ti = 0; ti < (int)tris.size(); ++ti) {
            TINTri t = tris[ti];
            const int e[3][2] = { {t.a,t.b},{t.b,t.c},{t.c,t.a} };
            for (int k = 0;k < 3;++k) {
                const int u = e[k][0], v = e[k][1];

                // пересечение AB и uv?
                const double A1 = B.y - A.y;
                const double B1 = A.x - B.x;
                const double C1 = A1 * A.x + B1 * A.y;

                const TINNode CU = nodes[u], CV = nodes[v];
                const double A2 = CV.y - CU.y;
                const double B2 = CU.x - CV.x;
                const double C2 = A2 * CU.x + B2 * CU.y;

                const double det = A1 * B2 - A2 * B1;
                if (std::fabs(det) < 1e-12) continue;

                const double ix = (B2 * C1 - B1 * C2) / det;
                const double iy = (A1 * C2 - A2 * C1) / det;

                auto between = [](double a, double b, double x) {
                    const double minv = std::min(a, b) - 1e-9, maxv = std::max(a, b) + 1e-9;
                    return x >= minv && x <= maxv;
                    };
                if (!between(A.x, B.x, ix) || !between(A.y, B.y, iy) ||
                    !between(CU.x, CV.x, ix) || !between(CU.y, CV.y, iy)) continue;

                const double len2 = (B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y);
                if (len2 < 1e-18) continue;
                const double tpar = ((ix - A.x) * (B.x - A.x) + (iy - A.y) * (B.y - A.y)) / len2;
                if (tpar <= 1e-9 || tpar >= 1.0 - 1e-9) continue;

                // вставим IP в ВСЕ треугольники, где есть ребро u-v
                const int ip = (int)nodes.size();
                const double iz = A.z + tpar * (B.z - A.z);
                nodes.push_back({ ix, iy, iz });

                auto holders = FindTrianglesWithEdge(tris, u, v);
                for (int h : holders) SplitTriangleOnEdge(nodes, tris, h, u, v, ip);

                ips.push_back({ tpar, ip });
                progressed = true;
                break;
            }
            if (progressed) break;
        }
    }

    std::sort(ips.begin(), ips.end(), [](const IP& a, const IP& b) { return a.t < b.t; });
    std::vector<int> chain; chain.reserve(ips.size() + 2U);
    chain.push_back(ia);
    for (const auto& p : ips) chain.push_back(p.idx);
    chain.push_back(ib);
    return chain;
}

// ================================================================
// BaseZ for mesh (этаж + смещение меша)
// ================================================================
static inline double GetMeshBaseZ(const API_Element& meshElem)
{
    double storyZ = 0.0;
    GetStoryLevelZ(meshElem.header.floorInd, storyZ);

    // !!! Если в вашей версии API у Mesh нет поля 'level', замените на корректное:
    // double meshLevel = meshElem.mesh.bottomOffset;
    double meshLevel = meshElem.mesh.level;

    return storyZ + meshLevel;
}

// ================================================================
// CDT: глобальная легализация (edge-flip) с учётом констрейнтов
// ================================================================
static void GlobalConstrainedDelaunayLegalize(std::vector<TINNode>& nodes,
    std::vector<TINTri>& tris,
    const EdgeSet& constraints)
{
    auto buildAdj = [&](std::map<Edge, std::vector<int>, EdgeLess>& adj) {
        adj.clear();
        for (int ti = 0; ti < (int)tris.size(); ++ti) {
            const TINTri& t = tris[ti];
            adj[MkE(t.a, t.b)].push_back(ti);
            adj[MkE(t.b, t.c)].push_back(ti);
            adj[MkE(t.c, t.a)].push_back(ti);
        }
        };

    int guard = 0;
    const int GUARD_MAX = 10000;
    bool flipped = true;
    while (flipped && guard++ < GUARD_MAX) {
        flipped = false;
        std::map<Edge, std::vector<int>, EdgeLess> adj;
        buildAdj(adj);

        for (const auto& kv : adj) {
            const Edge e = kv.first;
            const auto& owners = kv.second;
            if ((int)owners.size() != 2) continue;          // граничное ребро
            if (IsConstrained(constraints, e.u, e.v)) continue; // нельзя флипать констрейнт

            const int t0 = owners[0], t1 = owners[1];
            const TINTri& A = tris[t0];
            const TINTri& B = tris[t1];
            const int p = Opposite(A, e.u, e.v);
            const int q = Opposite(B, e.u, e.v);

            // локальный критерий Делоне
            const bool viol0 = InCircleCCW(nodes[e.u], nodes[e.v], nodes[p], nodes[q]);
            const bool viol1 = InCircleCCW(nodes[e.v], nodes[e.u], nodes[q], nodes[p]);
            if (!(viol0 || viol1)) continue;

            // проверим вырождение будущих треугольников
            TINTri NA{ p, e.u, q }; TINTri NB{ p, q, e.v };
            if (std::fabs(TriArea2D(nodes[NA.a], nodes[NA.b], nodes[NA.c])) < 1e-14) continue;
            if (std::fabs(TriArea2D(nodes[NB.a], nodes[NB.b], nodes[NB.c])) < 1e-14) continue;

            tris[t0] = NA; MakeCCW(nodes, tris[t0]);
            tris[t1] = NB; MakeCCW(nodes, tris[t1]);

            flipped = true;
            break; // перестроим карту владений после флипа
        }
    }
    if (guard >= GUARD_MAX) Log("[CDT] Global legalization reached guard limit.");
}

// ================================================================
// Build TIN and sample Z at pos3D.xy
// ================================================================
static bool BuildTIN_AndSampleZ(const API_Element& elem, const API_ElementMemo& memo,
    const API_Coord3D& pos3D,
    double& outZ, API_Vector3D& outN)
{
    const double baseZ = GetMeshBaseZ(elem);
    Log("[TIN] baseZ=%.6f", baseZ);

    // ------------------------------
    // 0) Проверка сложности меша
    // ------------------------------
    const int nCoords = elem.mesh.poly.nCoords;
    if (nCoords > 800) {
        Log("[TIN] Mesh too complex (nCoords=%d) — fallback to nearest vertex.", nCoords);

        // Быстрый вариант — ближайшая вершина по XY
        if (memo.coords && memo.meshPolyZ) {
            API_Coord* coords = *memo.coords;
            double* zs = *memo.meshPolyZ;
            int zCount = (int)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double));
            int stride = (zCount >= nCoords + 1) ? nCoords + 1 : nCoords;

            double bestDist = 1e12;
            double bestZ = 0.0;
            for (int i = 1; i <= nCoords - 1; ++i) {
                const API_Coord& c = coords[(stride == nCoords + 1) ? i : (i - 1)];
                double dx = pos3D.x - c.x;
                double dy = pos3D.y - c.y;
                double dist = std::sqrt(dx * dx + dy * dy);
                if (dist < bestDist) {
                    bestDist = dist;
                    int zi = (stride == nCoords + 1) ? i : (i - 1);
                    bestZ = zs[zi];
                }
            }
            outZ = baseZ + bestZ;
            outN = { 0,0,1 };
            Log("[TIN] Fallback Z=%.6f (nearest vertex)", outZ);
            return true;
        }
        return false;
    }

    // ------------------------------
    // 1) Контур и базовая триангуляция
    // ------------------------------
    MeshPolyData mp = BuildContourNodes(elem, memo, baseZ);
    if (!mp.ok || mp.contour.size() < 3) {
        Log("[TIN] contour build failed");
        return false;
    }

    // Очистка дубликатов (по XY)
    std::vector<TINNode> clean;
    for (const auto& p : mp.contour) {
        bool dup = false;
        for (const auto& q : clean) {
            if (std::fabs(p.x - q.x) < 1.0 && std::fabs(p.y - q.y) < 1.0) { dup = true; break; }
        }
        if (!dup) clean.push_back(p);
    }
    mp.contour.swap(clean);

    std::vector<TINNode> nodes = mp.contour;
    std::vector<TINTri> tris = TriangulateEarClipping(mp.contour);
    if (tris.empty()) {
        Log("[TIN] Triangulation failed (degenerate polygon)");
        return false;
    }

    // ------------------------------
    // 2) Добавляем level-точки, если их немного
    // ------------------------------
    const int lvlCnt = (memo.meshLevelCoords)
        ? (int)(BMGetHandleSize((GSHandle)memo.meshLevelCoords) / sizeof(API_MeshLevelCoord))
        : 0;

    if (lvlCnt > 0 && lvlCnt < 100) {   // ограничитель по уровню
        API_MeshLevelCoord* lvl = *memo.meshLevelCoords;
        Log("[TIN] Level points=%d", lvlCnt);
        for (int i = 0; i < lvlCnt; ++i) {
            TINNode P{ lvl[i].c.x, lvl[i].c.y, lvl[i].c.z };

            // Если точка внутри или на краю — вставляем
            int triIdx = FindTriContaining(nodes, tris, P);
            if (triIdx >= 0) {
                int pIdx = (int)nodes.size();
                nodes.push_back(P);
                int t0, t1, t2;
                SplitTriByPoint(nodes, tris, triIdx, pIdx, t0, t1, t2);
                Log("[TIN] Added level point #%d inside tri=%d", i + 1, triIdx);
            }
        }
    }
    else if (lvlCnt >= 100) {
        Log("[TIN] Too many level points (%d) — ignored.", lvlCnt);
    }

    // ------------------------------
    // 3) Быстрая Delaunay-легализация
    // ------------------------------
    EdgeSet constraints;
    for (int i = 0; i < (int)mp.contour.size(); ++i)
        constraints.insert(MkE(i, (i + 1) % (int)mp.contour.size()));
    GlobalConstrainedDelaunayLegalize(nodes, tris, constraints);

    // ------------------------------
    // 4) Семплинг Z по барицентрам
    // ------------------------------
    TINNode P{ pos3D.x, pos3D.y, 0.0 };
    const int triHit = FindTriContaining(nodes, tris, P);
    if (triHit < 0) {
        Log("[TIN] point outside TIN (fallback to nearest vertex)");
        // fallback 2 — ближайшая вершина
        double best = 1e12; double bestZ = baseZ;
        for (auto& n : nodes) {
            double d = std::hypot(pos3D.x - n.x, pos3D.y - n.y);
            if (d < best) { best = d; bestZ = n.z; }
        }
        outZ = bestZ; outN = { 0,0,1 };
        return true;
    }

    const TINTri& t = tris[triHit];
    double wA, wB, wC;
    BaryXY(P, nodes[t.a], nodes[t.b], nodes[t.c], wA, wB, wC);
    outZ = wA * nodes[t.a].z + wB * nodes[t.b].z + wC * nodes[t.c].z;
    outN = TriNormal3D(nodes[t.a], nodes[t.b], nodes[t.c]);

    Log("[TIN] OK z=%.6f tri=(%d,%d,%d)", outZ, t.a, t.b, t.c);
    return true;
}

// ================================================================
// MEMO-only ground Z via TIN
// ================================================================


static bool ComputeGroundZ_MemoOnly(const API_Guid& meshGuid, const API_Coord3D& pos3D,
    double& outAbsZ, API_Vector3D& outNormal)
{
    outAbsZ = 0.0; outNormal = { 0, 0, 1 };

    // 🔥 добавляем вот тут:
    API_ElementMemo freshMemo{};
    ACAPI_Element_GetMemo(meshGuid, &freshMemo, APIMemoMask_All);
    Log("[DEBUG] refreshed mesh memo after edit");
    ACAPI_DisposeElemMemoHdls(&freshMemo);
    // 🔥 конец вставки

    API_Element elem{}; elem.header.guid = meshGuid;
    if (ACAPI_Element_Get(&elem) != NoError) { Log("[TIN] Element_Get failed for mesh."); return false; }

    API_ElementMemo memo{};

    if (ACAPI_Element_GetMemo(meshGuid, &memo,
        APIMemoMask_MeshLevel | APIMemoMask_Polygon | APIMemoMask_MeshPolyZ) != NoError)
    {
        Log("[TIN] Element_GetMemo failed."); return false;
    }

    bool ok = BuildTIN_AndSampleZ(elem, memo, pos3D, outAbsZ, outNormal);
    ACAPI_DisposeElemMemoHdls(&memo);
    return ok;
}

// ================================================================
// Public API
// ================================================================
bool GroundHelper::SetGroundSurface()
{
    Log("[SetGroundSurface] ENTER");
    g_surfaceGuid = APINULLGuid;

    API_SelectionInfo selInfo{}; GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    Log("[SetGroundSurface] Selected neigs count=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{}; el.header.guid = n.guid;
        const GSErr err = ACAPI_Element_Get(&el);
        Log("[SetGroundSurface] neig guid=%s typeID=%d err=%d",
            APIGuidToString(n.guid).ToCStr().Get(), (int)el.header.type.typeID, (int)err);
        if (err != NoError) continue;
        if (el.header.type.typeID == API_MeshID) {
            g_surfaceGuid = n.guid;
            Log("[SetGroundSurface] Mesh selected: %s", APIGuidToString(n.guid).ToCStr().Get());

            LogMesh2DCoords(g_surfaceGuid);

#if ENABLE_PROBE_ADD_POINT
            Probe_AddLevelPointAt(g_surfaceGuid, 0.0, 0.0, 0.0);
#endif
            break;
        }
    }

    if (g_surfaceGuid == APINULLGuid) {
        Log("[SetGroundSurface] No mesh selected");
        return false;
    }
    Log("[SetGroundSurface] EXIT (true)");
    return true;
}

bool GroundHelper::SetGroundObjects()
{
    Log("[SetGroundObjects] ENTER");
    g_objectGuids.Clear();

    API_SelectionInfo selInfo{}; GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    Log("[SetGroundObjects] Selected neigs count=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{}; el.header.guid = n.guid;
        if (ACAPI_Element_Get(&el) != NoError) continue;
        const short tid = el.header.type.typeID;
        Log("[SetGroundObjects] neig guid=%s -> typeID=%d", APIGuidToString(n.guid).ToCStr().Get(), (int)tid);
        if ((tid == API_ObjectID || tid == API_LampID || tid == API_ColumnID) && n.guid != g_surfaceGuid) {
            g_objectGuids.Push(n.guid);
            Log("[SetGroundObjects] Will land: %s (type=%d)", APIGuidToString(n.guid).ToCStr().Get(), (int)tid);
        }
        else {
            Log("[SetGroundObjects] Skip guid=%s type=%d", APIGuidToString(n.guid).ToCStr().Get(), (int)tid);
        }
    }

    Log("[SetGroundObjects] Objects count=%u", (unsigned)g_objectGuids.GetSize());
    Log("[SetGroundObjects] EXIT %s", g_objectGuids.IsEmpty() ? "(false)" : "(true)");
    return !g_objectGuids.IsEmpty();
}

bool GroundHelper::GetGroundZAndNormal(const API_Coord3D& pos3D, double& z, API_Vector3D& normal)
{
    if (g_surfaceGuid == APINULLGuid) {
        Log("[GetGround] surface not set");
        return false;
    }
    Log("[GetGround] Call pos=(%.6f, %.6f, %.6f)", pos3D.x, pos3D.y, pos3D.z);
    return ComputeGroundZ_MemoOnly(g_surfaceGuid, pos3D, z, normal);
}

bool GroundHelper::ApplyGroundOffset(double offset /* meters */)
{
    Log("[ApplyGroundOffset] ENTER offset=%.6f", offset);
    if (g_surfaceGuid == APINULLGuid || g_objectGuids.IsEmpty()) {
        Log("[ApplyGroundOffset] no surface or no objects");
        return false;
    }

    const GSErrCode cmdErr = ACAPI_CallUndoableCommand("Ground Offset", [=]() -> GSErrCode {

        for (const API_Guid& guid : g_objectGuids) {
            Log("[Apply] process guid=%s", APIGuidToString(guid).ToCStr().Get());
            API_Element element{};
            if (!FetchElementByGuid(guid, element)) { Log("[Apply] FetchElement failed, skip"); continue; }

            double elemFloorZ = 0.0; GetStoryLevelZ(element.header.floorInd, elemFloorZ);
            API_Coord3D pos3D{ 0, 0, 0 };
            switch (element.header.type.typeID) {
            case API_ObjectID: pos3D = { element.object.pos.x,  element.object.pos.y,  elemFloorZ + element.object.level }; break;
            case API_LampID:   pos3D = { element.lamp.pos.x,    element.lamp.pos.y,    elemFloorZ + element.lamp.level }; break;
            case API_ColumnID: pos3D = { element.column.origoPos.x, element.column.origoPos.y, elemFloorZ + element.column.bottomOffset }; break;
            default: Log("[Apply] Unsupported type, skip"); continue;
            }

            Log("[Apply] pos3D=(%.6f, %.6f, %.6f) floorZ=%.6f", pos3D.x, pos3D.y, pos3D.z, elemFloorZ);

            double surfaceZ = 0.0; API_Vector3D n{ 0, 0, 1 };
            if (!ComputeGroundZ_MemoOnly(g_surfaceGuid, pos3D, surfaceZ, n)) {
                Log("[Apply] Could not get surface Z -> skip");
                continue;
            }

            const double finalZ = surfaceZ + offset;
            Log("[Apply] baseZ=%.6f finalZ=%.6f (offset=%.6f)", surfaceZ, finalZ, offset);

            API_Element mask{}; ACAPI_ELEMENT_MASK_CLEAR(mask);
            switch (element.header.type.typeID) {
            case API_ObjectID: element.object.level = finalZ - elemFloorZ; ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, level); break;
            case API_LampID:   element.lamp.level = finalZ - elemFloorZ;   ACAPI_ELEMENT_MASK_SET(mask, API_LampType, level); break;
            case API_ColumnID: element.column.bottomOffset = finalZ - elemFloorZ; ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, bottomOffset); break;
            }

            const GSErrCode chg = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
            if (chg == NoError) Log("[Apply] Updated guid=%s", APIGuidToString(guid).ToCStr().Get());
            else                Log("[Apply] Change failed err=%d guid=%s", (int)chg, APIGuidToString(guid).ToCStr().Get());
        }
        return NoError;
        });

    Log("[ApplyGroundOffset] EXIT (cmdErr=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}
