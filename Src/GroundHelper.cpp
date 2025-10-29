// GroundHelper.cpp — TIN-landing по Mesh (CDT + edge flips) + параноидальные логи
//
// ВАЖНО:
//  - ApplyGroundOffset() ИГНОРИРУЕТ параметр offset и ставит объекты ровно на Mesh.
//  - Для кнопки «Смещение» (ΔZ) в UI вызывайте ApplyZDelta(deltaMeters) — это чистый сдвиг по мировой Z.
//  - ФИКС: для колонн теперь двигаем и низ и верх (topOffset += ΔZ), чтобы высота не менялась.

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
#define ENABLE_PROBE_ADD_POINT 0   // 1 — врезать тестовую level-точку в Mesh

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
    API_StoryInfo si{}; const GSErr e = ACAPI_ProjectSetting_GetStorySettings(&si);
    if (e != NoError || si.data == nullptr) {
        Log("[Story] GetStorySettings failed err=%d", (int)e);
        return false;
    }
    const Int32 cnt = (Int32)(BMGetHandleSize((GSHandle)si.data) / sizeof(API_StoryType));
    if (floorInd >= 0 && cnt > 0) {
        const Int32 idx = floorInd - si.firstStory;
        if (0 <= idx && idx < cnt) outZ = (*si.data)[idx].level;
    }
    		// Log("[Story] floorInd=%d to storyZ=%.6f", (int)floorInd, outZ); // Убрали - слишком много логов
    BMKillHandle((GSHandle*)&si.data);
    return true;
}

// ================================================================
// Fetch element
// ================================================================
static bool FetchElementByGuid(const API_Guid& guid, API_Element& out)
{
    out = {}; out.header.guid = guid;

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
static inline double TriArea2Dxy(double ax, double ay, double bx, double by, double cx, double cy) {
    return 0.5 * Cross2D(ax, ay, bx, by, cx, cy);
}
static inline bool PointInTriStrictXY(double px, double py, double ax, double ay, double bx, double by, double cx, double cy) {
    const double c1 = Cross2D(ax, ay, bx, by, px, py);
    const double c2 = Cross2D(bx, by, cx, cy, px, py);
    const double c3 = Cross2D(cx, cy, ax, ay, px, py);
    return ((c1 > 0.0) && (c2 > 0.0) && (c3 > 0.0)) || ((c1 < 0.0) && (c2 < 0.0) && (c3 < 0.0));
}

// ================================================================
// Dump mesh plan data — для диагностики
// ================================================================
static void LogMesh2DCoords(const API_Guid& meshGuid)
{
    API_Element elem{}; elem.header.guid = meshGuid;
    if (ACAPI_Element_Get(&elem) != NoError) { Log("[Mesh2D] Element_Get failed"); return; }

    API_ElementMemo memo{};
    const GSErrCode em = ACAPI_Element_GetMemo(
        meshGuid, &memo, APIMemoMask_Polygon | APIMemoMask_MeshPolyZ | APIMemoMask_MeshLevel
    );
    if (em != NoError) { Log("[Mesh2D] GetMemo failed err=%d", (int)em); return; }

    const Int32 nCoords = elem.mesh.poly.nCoords;                 // includes closing vertex
    const API_Coord* coordsH = memo.coords ? *memo.coords : nullptr;
    const double* zH = memo.meshPolyZ ? *memo.meshPolyZ : nullptr;
    const Int32 zCount = zH ? (Int32)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double)) : 0;

    Log("[Mesh2D] nSubPolys=%d nCoords=%d zCount=%d", (int)elem.mesh.poly.nSubPolys, (int)nCoords, (int)zCount);

    if (coordsH && zH) {
        const bool coords1 = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) == nCoords + 1;
        const bool z1 = (zCount % (nCoords + 1) == 0);
        for (Int32 i = 1; i <= nCoords - 1; ++i) {
            const API_Coord& c = coordsH[coords1 ? i : (i - 1)];
            const Int32 zi = z1 ? i : (i - 1);
            Log("[Mesh2D]  #%d: (%.6f, %.6f) zLocal=%.6f", (int)i, c.x, c.y, zH[zi]);
        }
    }
    else {
        Log("[Mesh2D] coords or zH is null");
    }
    ACAPI_DisposeElemMemoHdls(&memo);
}

// ================================================================
// Probe: add level point (debug)
// ================================================================
static bool Probe_AddLevelPointAt(const API_Guid& meshGuid, double x, double y, double z)
{
#if ENABLE_PROBE_ADD_POINT
    // ... (оставил как у вас; код опущен ради компактности) ...
#else
    (void)meshGuid; (void)x; (void)y; (void)z;
#endif
    return true;
}

// ================================================================
// TIN structures
// ================================================================
struct TINNode { double x, y, z; };
struct TINTri { int a, b, c; };

static inline double TriArea2D(const TINNode& A, const TINNode& B, const TINNode& C) {
    return TriArea2Dxy(A.x, A.y, B.x, B.y, C.x, C.y);
}
static inline bool IsCCW_Poly(const std::vector<TINNode>& poly) {
    double A = 0.0;
    for (size_t i = 0, n = poly.size(); i < n; ++i) {
        const TINNode& p = poly[i], & q = poly[(i + 1) % n];
        A += p.x * q.y - p.y * q.x;
    }
    return A > 0.0;
}
static inline API_Vector3D TriNormal3D(const TINNode& A, const TINNode& B, const TINNode& C) {
    const double ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
    const double vx = C.x - A.x, vy = C.y - A.y, vz = C.z - A.z;
    API_Vector3D n{ uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
    Normalize(n); if (n.z < 0.0) { n.x = -n.x; n.y = -n.y; n.z = -n.z; }
    return n;
}

// ================================================================
// Ear clipping
// ================================================================
static std::vector<TINTri> TriangulateEarClipping(const std::vector<TINNode>& poly) {
    std::vector<TINTri> tris; const size_t n = poly.size();
    if (n < 3) return tris;
    std::vector<int> idx(n); for (size_t i = 0; i < n; ++i) idx[i] = (int)i;

    const bool ccw = IsCCW_Poly(poly);
    auto isConvex = [&](int i0, int i1, int i2) {
        const TINNode& A = poly[idx[i0]], & B = poly[idx[i1]], & C = poly[idx[i2]];
        const double cross = Cross2D(A.x, A.y, B.x, B.y, C.x, C.y);
        return ccw ? (cross > 0.0) : (cross < 0.0);
        };

    size_t guard = 0;
    while (idx.size() > 3 && guard++ < n * n) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            const int i0 = (int)((i + idx.size() - 1) % idx.size());
            const int i1 = (int)i;
            const int i2 = (int)((i + 1) % idx.size());
            if (!isConvex(i0, i1, i2)) continue;

            const TINNode& A = poly[idx[i0]], & B = poly[idx[i1]], & C = poly[idx[i2]];
            bool empty = true;
            for (size_t k = 0; k < idx.size(); ++k) {
                if (k == (size_t)i0 || k == (size_t)i1 || k == (size_t)i2) continue;
                if (PointInTriStrictXY(poly[idx[k]].x, poly[idx[k]].y, A.x, A.y, B.x, B.y, C.x, C.y)) {
                    empty = false; break;
                }
            }
            if (!empty) continue;
            TINTri t{ idx[i0], idx[i1], idx[i2] }; if (!ccw) std::swap(t.b, t.c);
            tris.push_back(t); idx.erase(idx.begin() + i1); clipped = true; break;
        }
        if (!clipped) break;
    }
    if (idx.size() == 3) {
        TINTri t{ idx[0], idx[1], idx[2] };
        if (TriArea2D(poly[t.a], poly[t.b], poly[t.c]) < 0.0) std::swap(t.b, t.c);
        tris.push_back(t);
    }
    return tris;
}

// ================================================================
// Find tri & split by Steiner point
// ================================================================
static int FindTriContaining(const std::vector<TINNode>& nodes,
    const std::vector<TINTri>& tris,
    const TINNode& P)
{
    for (int ti = 0; ti < (int)tris.size(); ++ti) {
        const TINTri& t = tris[ti];
        const TINNode& A = nodes[t.a], & B = nodes[t.b], & C = nodes[t.c];
        const bool inside = !(
            (Cross2D(A.x, A.y, B.x, B.y, P.x, P.y) < 0.0) ||
            (Cross2D(B.x, B.y, C.x, C.y, P.x, P.y) < 0.0) ||
            (Cross2D(C.x, C.y, A.x, A.y, P.x, P.y) < 0.0)
            );
        if (inside) return ti;
    }
    return -1;
}
static void SplitTriByPoint(std::vector<TINNode>& nodes, std::vector<TINTri>& tris,
    int triIndex, int pIdx, int& t0, int& t1, int& t2)
{
    const TINTri t = tris[triIndex];
    TINTri A{ t.a, t.b, pIdx }, B{ t.b, t.c, pIdx }, C{ t.c, t.a, pIdx };
    if (TriArea2D(nodes[A.a], nodes[A.b], nodes[A.c]) < 0.0) std::swap(A.b, A.c);
    if (TriArea2D(nodes[B.a], nodes[B.b], nodes[B.c]) < 0.0) std::swap(B.b, B.c);
    if (TriArea2D(nodes[C.a], nodes[C.b], nodes[C.c]) < 0.0) std::swap(C.b, C.c);
    tris[triIndex] = A; t0 = triIndex; tris.push_back(B); t1 = (int)tris.size() - 1; tris.push_back(C); t2 = (int)tris.size() - 1;
}

// ================================================================
// Mesh base Z (этажа + собственный сдвиг меша)
// ================================================================
static inline double GetMeshBaseZ(const API_Element& meshElem)
{
    double storyZ = 0.0; GetStoryLevelZ(meshElem.header.floorInd, storyZ);

    // ВНИМАНИЕ: в разных версиях API вертикальная привязка Mesh хранится в разных полях.
    // Если у вас «уровень меша» это bottomOffset — раскомментируйте следующую строку
    // и закомментируйте level (или наоборот).
    // const double meshLevel = meshElem.mesh.bottomOffset;
    const double meshLevel = meshElem.mesh.level;

    const double baseZ = storyZ + meshLevel;
    	// Log("[MeshBase] floorInd=%d storyZ=%.6f meshLevel=%.6f to baseZ=%.6f", // Убрали - слишком много логов
                 // (int)meshElem.header.floorInd, storyZ, meshLevel, baseZ); // Убрали - слишком много логов
    return baseZ;
}

// ================================================================
// CDT legalization (с констрейнтами по контуру)
// ================================================================
struct Edge { int u, v; };
static inline Edge MkE(int a, int b) { if (a > b) std::swap(a, b); return { a, b }; }
struct EdgeLess { bool operator()(const Edge& a, const Edge& b) const { return a.u < b.u || (a.u == b.u && a.v < b.v); } };
using EdgeSet = std::set<Edge, EdgeLess>;

static inline bool InCircleCCW(const TINNode& A, const TINNode& B, const TINNode& C, const TINNode& P)
{
    double ax = A.x - P.x, ay = A.y - P.y;
    double bx = B.x - P.x, by = B.y - P.y;
    double cx = C.x - P.x, cy = C.y - P.y;
    double det = (ax * ax + ay * ay) * (bx * cy - by * cx)
        - (bx * bx + by * by) * (ax * cy - ay * cx)
        + (cx * cx + cy * cy) * (ax * by - ay * bx);
    const double areaABC = Cross2D(A.x, A.y, B.x, B.y, C.x, C.y);
    if (areaABC < 0.0) det = -det;
    return det > 0.0;
}
static inline int Opposite(const TINTri& t, int u, int v) {
    if (t.a != u && t.a != v) return t.a;
    if (t.b != u && t.b != v) return t.b;
    return t.c;
}
static inline void MakeCCW(const std::vector<TINNode>& nodes, TINTri& t)
{
    if (TriArea2D(nodes[t.a], nodes[t.b], nodes[t.c]) < 0.0) std::swap(t.b, t.c);
}

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

    int guard = 0; const int GUARD_MAX = 10000; bool flipped = true;
    while (flipped && guard++ < GUARD_MAX) {
        flipped = false;
        std::map<Edge, std::vector<int>, EdgeLess> adj; buildAdj(adj);
        for (const auto& kv : adj) {
            const Edge e = kv.first; const auto& owners = kv.second;
            if ((int)owners.size() != 2) continue;
            if (constraints.count(e) > 0) continue;

            const int t0 = owners[0], t1 = owners[1];
            const TINTri& A = tris[t0]; const TINTri& B = tris[t1];
            const int p = Opposite(A, e.u, e.v), q = Opposite(B, e.u, e.v);

            const bool viol = InCircleCCW(nodes[e.u], nodes[e.v], nodes[p], nodes[q])
                || InCircleCCW(nodes[e.v], nodes[e.u], nodes[q], nodes[p]);
            if (!viol) continue;

            TINTri NA{ p, e.u, q }, NB{ p, q, e.v };
            if (std::fabs(TriArea2D(nodes[NA.a], nodes[NA.b], nodes[NA.c])) < 1e-14) continue;
            if (std::fabs(TriArea2D(nodes[NB.a], nodes[NB.b], nodes[NB.c])) < 1e-14) continue;

            tris[t0] = NA; MakeCCW(nodes, tris[t0]);
            tris[t1] = NB; MakeCCW(nodes, tris[t1]);
            flipped = true; break;
        }
    }
    if (guard >= GUARD_MAX) Log("[CDT] legalization reached guard limit");
}

// ================================================================
// Build contour nodes (outer) with absolute Z
// ================================================================
struct MeshPolyData { std::vector<TINNode> contour; bool ok = false; };

static MeshPolyData BuildContourNodes(const API_Element& elem, const API_ElementMemo& memo, double baseZ) {
    MeshPolyData out{};
    if (memo.coords == nullptr || memo.meshPolyZ == nullptr || elem.mesh.poly.nCoords < 3) return out;

    const API_Coord* coords = *memo.coords;
    const Int32 nCoords = elem.mesh.poly.nCoords; // includes closing
    const bool coords1 = ((Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) == nCoords + 1);

    const double* zH = *memo.meshPolyZ;
    const Int32 zCount = (Int32)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double));
    Int32 strideZ = (zCount % (nCoords + 1) == 0) ? (nCoords + 1) : nCoords;
    const bool z1 = (strideZ == nCoords + 1);

    out.contour.reserve((size_t)(nCoords - 1));
    for (Int32 i = 1; i <= nCoords - 1; ++i) {
        const API_Coord& c = coords[coords1 ? i : (i - 1)];
        const Int32 zi = z1 ? i : (i - 1);
        const double absZ = baseZ + zH[zi];
        out.contour.push_back({ c.x, c.y, absZ });
    }
    out.ok = true;
    	// Log("[Contour] built nodes=%zu", out.contour.size()); // Убрали - слишком много логов
    return out;
}

// ================================================================
// TIN build & sample Z
// ================================================================
static inline void BaryXY(const TINNode& P, const TINNode& A, const TINNode& B, const TINNode& C,
    double& wA, double& wB, double& wC)
{
    const double areaABC = TriArea2D(A, B, C);
    if (std::fabs(areaABC) < 1e-14) { wA = wB = wC = 0.0; return; }
    const double areaPBC = TriArea2D(P, B, C);
    const double areaPCA = TriArea2D(P, C, A);
    wA = areaPBC / areaABC; wB = areaPCA / areaABC; wC = 1.0 - wA - wB;
}

static bool BuildTIN_AndSampleZ(const API_Element& elem, const API_ElementMemo& memo,
    const API_Coord3D& pos3D,
    double& outZ, API_Vector3D& outN)
{
    const double baseZ = GetMeshBaseZ(elem);

    // сложный Mesh → fallback
    if (elem.mesh.poly.nCoords > 800) {
        Log("[TIN] too complex (nCoords=%d) to fallback to nearest vertex", (int)elem.mesh.poly.nCoords);
        if (memo.coords && memo.meshPolyZ) {
            const API_Coord* coords = *memo.coords;
            const double* zs = *memo.meshPolyZ;
            const Int32 nCoords = elem.mesh.poly.nCoords;
            const bool coords1 = ((Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) == nCoords + 1);
            const Int32 zCount = (Int32)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double));
            const bool z1 = (zCount % (nCoords + 1) == 0);
            double best = 1e12, bestZ = baseZ;
            for (int i = 1; i <= nCoords - 1; ++i) {
                const API_Coord& c = coords[coords1 ? i : (i - 1)];
                const double d = std::hypot(pos3D.x - c.x, pos3D.y - c.y);
                if (d < best) { best = d; bestZ = baseZ + zs[z1 ? i : (i - 1)]; }
            }
            outZ = bestZ; outN = { 0,0,1 }; Log("[TIN] fallback Z=%.6f", outZ);
            return true;
        }
        return false;
    }

    MeshPolyData mp = BuildContourNodes(elem, memo, baseZ);
    if (!mp.ok || mp.contour.size() < 3) { Log("[TIN] contour build failed"); return false; }

    // очистка дублей на всякий
    std::vector<TINNode> nodes; nodes.reserve(mp.contour.size());
    for (const auto& p : mp.contour) {
        bool dup = false;
        for (const auto& q : nodes) if (std::fabs(p.x - q.x) < 1.0 && std::fabs(p.y - q.y) < 1.0) { dup = true; break; }
        if (!dup) nodes.push_back(p);
    }
    std::vector<TINTri> tris = TriangulateEarClipping(nodes);
    if (tris.empty()) { Log("[TIN] triangulation failed"); return false; }

    // level-точки
    const int lvlCnt = memo.meshLevelCoords ? (int)(BMGetHandleSize((GSHandle)memo.meshLevelCoords) / sizeof(API_MeshLevelCoord)) : 0;
    if (lvlCnt > 0 && lvlCnt < 100) {
        const API_MeshLevelCoord* lvl = *memo.meshLevelCoords;
        		// Log("[TIN] level points: %d", lvlCnt); // Убрали - слишком много логов
        for (int i = 0; i < lvlCnt; ++i) {
            const TINNode P{ lvl[i].c.x, lvl[i].c.y, lvl[i].c.z };
            const int triIdx = FindTriContaining(nodes, tris, P);
            if (triIdx >= 0) {
                const int pIdx = (int)nodes.size(); nodes.push_back(P);
                int t0, t1, t2; SplitTriByPoint(nodes, tris, triIdx, pIdx, t0, t1, t2);
                			// Log("[TIN]  add level #%d in tri=%d", i + 1, triIdx); // Убрали - слишком много логов
            }
        }
    }
    else if (lvlCnt >= 100) Log("[TIN] too many level points (%d) will ignored", lvlCnt);

    // Делоне-легализация
    EdgeSet constraints; for (int i = 0, n = (int)nodes.size(); i < (int)mp.contour.size(); ++i)
        constraints.insert(MkE(i, (i + 1) % (int)mp.contour.size()));
    GlobalConstrainedDelaunayLegalize(nodes, tris, constraints);

    // семплинг
    const TINNode P{ pos3D.x, pos3D.y, 0.0 };
    const int triHit = FindTriContaining(nodes, tris, P);
    if (triHit < 0) {
        Log("[TIN] point outside, fallback to nearest vertex");
        double best = 1e12, bestZ = baseZ;
        for (const auto& n : nodes) { const double d = std::hypot(P.x - n.x, P.y - n.y); if (d < best) { best = d; bestZ = n.z; } }
        outZ = bestZ; outN = { 0,0,1 };
        return true;
    }
    const TINTri& t = tris[triHit];
    double wA, wB, wC; BaryXY(P, nodes[t.a], nodes[t.b], nodes[t.c], wA, wB, wC);
    outZ = wA * nodes[t.a].z + wB * nodes[t.b].z + wC * nodes[t.c].z;
    outN = TriNormal3D(nodes[t.a], nodes[t.b], nodes[t.c]);
    		// Log("[TIN] hit tri=(%d,%d,%d) to Z=%.6f", t.a, t.b, t.c, outZ); // Убрали - слишком много логов
    return true;
}

// ================================================================
// Compute ground Z (memo only)
// ================================================================
static bool ComputeGroundZ_MemoOnly(const API_Guid& meshGuid, const API_Coord3D& pos3D,
    double& outAbsZ, API_Vector3D& outNormal)
{
    outAbsZ = 0.0; outNormal = { 0,0,1 };

    API_Element elem{}; elem.header.guid = meshGuid;
    if (ACAPI_Element_Get(&elem) != NoError) { Log("[TIN] Element_Get(mesh) failed"); return false; }

    API_ElementMemo memo{};
    const GSErr mErr = ACAPI_Element_GetMemo(meshGuid, &memo,
        APIMemoMask_MeshLevel | APIMemoMask_Polygon | APIMemoMask_MeshPolyZ);
    if (mErr != NoError) { Log("[TIN] GetMemo failed err=%d", (int)mErr); return false; }

    const bool ok = BuildTIN_AndSampleZ(elem, memo, pos3D, outAbsZ, outNormal);
    ACAPI_DisposeElemMemoHdls(&memo);
    return ok;
}

// ================================================================
// Landable elements
// ================================================================
enum class LandableKind { Unsupported, Object, Lamp, Column };

static LandableKind IdentifyLandable(const API_Element& e)
{
    switch (e.header.type.typeID) {
    case API_ObjectID: return LandableKind::Object;
    case API_LampID:   return LandableKind::Lamp;
    case API_ColumnID: return LandableKind::Column;
    default:           return LandableKind::Unsupported;
    }
}

static API_Coord3D GetWorldAnchor(const API_Element& e)
{
    double floorZ = 0.0; GetStoryLevelZ(e.header.floorInd, floorZ);
    switch (IdentifyLandable(e)) {
    case LandableKind::Object: return { e.object.pos.x,   e.object.pos.y,   floorZ + e.object.level };
    case LandableKind::Lamp:   return { e.lamp.pos.x,     e.lamp.pos.y,     floorZ + e.lamp.level };
    case LandableKind::Column: return { e.column.origoPos.x, e.column.origoPos.y, floorZ + e.column.bottomOffset };
    default: return { 0,0,0 };
    }
}

// ФИКС: устанавливаем новый низ И поднимаем верх на тот же ΔZ (для колонны сохраняем высоту)
static void SetWorldZ_WithDelta(API_Element& e, double finalWorldZ, double deltaWorldZ, API_Element& maskOut)
{
    ACAPI_ELEMENT_MASK_CLEAR(maskOut);
    double floorZ = 0.0; GetStoryLevelZ(e.header.floorInd, floorZ);

    switch (IdentifyLandable(e)) {
    case LandableKind::Object: {
        const double old = e.object.level;
        e.object.level = finalWorldZ - floorZ;
        ACAPI_ELEMENT_MASK_SET(maskOut, API_ObjectType, level);
        Log("[SetZ:Object] old level=%.6f then new=%.6f (delta=%.6f)", old, e.object.level, deltaWorldZ);
        break;
    }
    case LandableKind::Lamp: {
        const double old = e.lamp.level;
        e.lamp.level = finalWorldZ - floorZ;
        ACAPI_ELEMENT_MASK_SET(maskOut, API_LampType, level);
        Log("[SetZ:Lamp] old level=%.6f then new=%.6f (delta=%.6f)", old, e.lamp.level, deltaWorldZ);
        break;
    }
    case LandableKind::Column: {
        const double oldBot = e.column.bottomOffset;
        const double oldTop = e.column.topOffset; // используем если доступно
        e.column.bottomOffset = finalWorldZ - floorZ;
        e.column.topOffset = oldTop + deltaWorldZ; // сохраняем высоту колонны
        ACAPI_ELEMENT_MASK_SET(maskOut, API_ColumnType, bottomOffset);
        ACAPI_ELEMENT_MASK_SET(maskOut, API_ColumnType, topOffset);
        Log("[SetZ:Column] bottom: %.6fto%.6f, topOffset: %.6fto%.6f (delta=%.6f)",
            oldBot, e.column.bottomOffset, oldTop, e.column.topOffset, deltaWorldZ);
        break;
    }
    default: break;
    }
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

    Log("[SetGroundSurface] neigs=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{}; el.header.guid = n.guid;
        const GSErr err = ACAPI_Element_Get(&el);
        Log("[SetGroundSurface] guid=%s typeID=%d err=%d",
            APIGuidToString(n.guid).ToCStr().Get(), (int)el.header.type.typeID, (int)err);
        if (err != NoError) continue;
        if (el.header.type.typeID == API_MeshID) {
            g_surfaceGuid = n.guid;
            Log("[SetGroundSurface] Mesh set: %s", APIGuidToString(n.guid).ToCStr().Get());
            LogMesh2DCoords(g_surfaceGuid);
            Probe_AddLevelPointAt(g_surfaceGuid, 0.0, 0.0, 0.0);
            break;
        }
    }

    const bool ok = (g_surfaceGuid != APINULLGuid);
    Log("[SetGroundSurface] EXIT %s", ok ? "true" : "false");
    return ok;
}

bool GroundHelper::SetGroundObjects()
{
    Log("[SetGroundObjects] ENTER");
    g_objectGuids.Clear();

    API_SelectionInfo selInfo{}; GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    Log("[SetGroundObjects] neigs=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{}; el.header.guid = n.guid;
        if (ACAPI_Element_Get(&el) != NoError) continue;
        const short tid = el.header.type.typeID;

        if ((tid == API_ObjectID || tid == API_LampID || tid == API_ColumnID) && n.guid != g_surfaceGuid) {
            g_objectGuids.Push(n.guid);
            Log("[SetGroundObjects] accept %s (type=%d)", APIGuidToString(n.guid).ToCStr().Get(), (int)tid);
        }
        else {
            Log("[SetGroundObjects] skip %s type=%d", APIGuidToString(n.guid).ToCStr().Get(), (int)tid);
        }
    }

    Log("[SetGroundObjects] COUNT=%u", (unsigned)g_objectGuids.GetSize());
    const bool ok = !g_objectGuids.IsEmpty();
    Log("[SetGroundObjects] EXIT %s", ok ? "true" : "false");
    return ok;
}

bool GroundHelper::GetGroundZAndNormal(const API_Coord3D& pos3D, double& z, API_Vector3D& normal)
{
    if (g_surfaceGuid == APINULLGuid) { Log("[GetGround] surface not set"); return false; }
    	// Log("[GetGround] pos=(%.6f, %.6f, %.6f)", pos3D.x, pos3D.y, pos3D.z); // Убрали - слишком много логов
    return ComputeGroundZ_MemoOnly(g_surfaceGuid, pos3D, z, normal);
}

bool GroundHelper::ApplyGroundOffset(double offset /* meters */)
{
    (void)offset; // намеренно игнорируем
    Log("[ApplyGroundOffset] ENTER (offset ignored)");
    if (g_surfaceGuid == APINULLGuid || g_objectGuids.IsEmpty()) { Log("[ApplyGroundOffset] no surface or no objects"); return false; }

    const GSErr cmdErr = ACAPI_CallUndoableCommand("Land to Mesh", [=]() -> GSErr {
        for (const API_Guid& guid : g_objectGuids) {
            API_Element e{}; if (!FetchElementByGuid(guid, e)) { Log("[Apply] fetch failed"); continue; }
            const LandableKind kind = IdentifyLandable(e); if (kind == LandableKind::Unsupported) { Log("[Apply] unsupported"); continue; }

            const API_Coord3D anchor = GetWorldAnchor(e);
            Log("[Apply] guid=%s kind=%d floorInd=%d anchor=(%.6f,%.6f,%.6f)",
                APIGuidToString(guid).ToCStr().Get(), (int)kind, (int)e.header.floorInd, anchor.x, anchor.y, anchor.z);

            double surfaceZ = 0.0; API_Vector3D n{ 0,0,1 };
            if (!ComputeGroundZ_MemoOnly(g_surfaceGuid, anchor, surfaceZ, n)) { Log("[Apply] can't sample surface Z will skip"); continue; }

            const double delta = surfaceZ - anchor.z; // КЛЮЧ: двигаем на Δ между текущим Z и поверхностью
            const double finalZ = anchor.z + delta;
            Log("[Apply] surfaceZ=%.6f  anchorZ=%.6f  delta=%.6f then finalZ=%.6f", surfaceZ, anchor.z, delta, finalZ);

            API_Element mask{};
            SetWorldZ_WithDelta(e, finalZ, delta, mask);

            const GSErr chg = ACAPI_Element_Change(&e, &mask, nullptr, 0, true);
            if (chg == NoError) Log("[Apply] UPDATED %s", APIGuidToString(guid).ToCStr().Get());
            else                Log("[Apply] Change FAILED err=%d %s", (int)chg, APIGuidToString(guid).ToCStr().Get());
        }
        return NoError;
        });

    Log("[ApplyGroundOffset] EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

bool GroundHelper::ApplyZDelta(double deltaMeters)
{
    Log("[ApplyZDelta] ENTER delta=%.6f", deltaMeters);
    
    // Если объекты не установлены, попробуем получить их из текущего выделения
    if (g_objectGuids.IsEmpty()) {
        Log("[ApplyZDelta] no objects in cache, trying to get from current selection");
        if (!SetGroundObjects()) {
            Log("[ApplyZDelta] no objects in selection either");
            return false;
        }
    }

    const GSErr cmdErr = ACAPI_CallUndoableCommand("Adjust Z by Delta", [=]() -> GSErr {
        for (const API_Guid& guid : g_objectGuids) {
            API_Element e{}; if (!FetchElementByGuid(guid, e)) { Log("[dltZ] fetch failed"); continue; }
            if (IdentifyLandable(e) == LandableKind::Unsupported) { Log("[dltZ] unsupported"); continue; }

            const API_Coord3D anchor = GetWorldAnchor(e);
            const double finalZ = anchor.z + deltaMeters;
            Log("[DeltaZ] guid=%s oldZ=%.6f to newZ=%.6f (dlt=%.6f)",
                APIGuidToString(guid).ToCStr().Get(), anchor.z, finalZ, deltaMeters);

            API_Element mask{};
            SetWorldZ_WithDelta(e, finalZ, deltaMeters, mask);

            const GSErr chg = ACAPI_Element_Change(&e, &mask, nullptr, 0, true);
            if (chg == NoError) Log("[DeltaZ] UPDATED %s", APIGuidToString(guid).ToCStr().Get());
            else                Log("[DeltaZ] Change FAILED err=%d %s", (int)chg, APIGuidToString(guid).ToCStr().Get());
        }
        return NoError;
        });

    Log("[ApplyZDelta] EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

bool GroundHelper::DebugOneSelection()
{
    Log("[DebugOneSelection] not implemented");
    return false;
}
