// ============================================================================
// GroundHelper.cpp — посадка объектов на Mesh через TIN (CDT + raycast)
// Archicad 27: у API_MeshType нет bottomOffset — используем только mesh.level
// ============================================================================

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
#define ENABLE_PROBE_ADD_POINT   0
#define MAX_LEVEL_POINTS      5000  // лимит level-точек из Mesh

// ------------------ Globals ------------------
static API_Guid g_surfaceGuid = APINULLGuid;
static GS::Array<API_Guid> g_objectGuids;

// ------------------ Logging ------------------
static inline void Log(const char* fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    char buf[4096];
    std::vsnprintf(buf, sizeof(buf), fmt, vl);  // ← было: vsnprintf(buf, sizeof(buf), vl)
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
static inline double Cross2D(double ax, double ay, double bx, double by, double cx, double cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}
static inline double TriArea2Dxy(double ax, double ay, double bx, double by, double cx, double cy)
{
    return 0.5 * Cross2D(ax, ay, bx, by, cx, cy);
}
static inline bool PointInTriStrictXY(double px, double py, double ax, double ay, double bx, double by, double cx, double cy, double eps = 1e-12)
{
    const double c1 = Cross2D(ax, ay, bx, by, px, py);
    const double c2 = Cross2D(bx, by, cx, cy, px, py);
    const double c3 = Cross2D(cx, cy, ax, ay, px, py);
    const bool s1 = (c1 > -eps), s2 = (c2 > -eps), s3 = (c3 > -eps);
    const bool s1n = (c1 < +eps), s2n = (c2 < +eps), s3n = (c3 < +eps);
    return ((s1 && s2 && s3) || (s1n && s2n && s3n));
}

// ================================================================
// TIN structures
// ================================================================
struct TINNode { double x, y, z; };
struct TINTri { int a, b, c; };

// Кеш TIN для быстрого доступа
static std::vector<TINNode> g_cachedNodes;
static std::vector<TINTri> g_cachedTris;
static double g_cachedBaseZ = 0.0;
static API_Guid g_cachedMeshGuid = APINULLGuid;
static bool g_hasCachedTIN = false;

static inline double TriArea2D(const TINNode& A, const TINNode& B, const TINNode& C)
{
    return TriArea2Dxy(A.x, A.y, B.x, B.y, C.x, C.y);
}
static inline bool IsCCW_Poly(const std::vector<TINNode>& poly)
{
    double A = 0.0;
    for (size_t i = 0, n = poly.size(); i < n; ++i) {
        const TINNode& p = poly[i], & q = poly[(i + 1) % n];
        A += p.x * q.y - p.y * q.x;
    }
    return A > 0.0;
}
static inline API_Vector3D TriNormal3D(const TINNode& A, const TINNode& B, const TINNode& C)
{
    const double ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z;
    const double vx = C.x - A.x, vy = C.y - A.y, vz = C.z - A.z;
    API_Vector3D n{ uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
    Normalize(n); if (n.z < 0.0) { n.x = -n.x; n.y = -n.y; n.z = -n.z; }
    return n;
}

// ================================================================
// Ear clipping
// ================================================================
static std::vector<TINTri> TriangulateEarClipping(const std::vector<TINNode>& poly)
{
    std::vector<TINTri> tris; const size_t n = poly.size(); if (n < 3) return tris;
    std::vector<int> idx(n); for (size_t i = 0;i < n;++i) idx[i] = (int)i;

    const bool ccw = IsCCW_Poly(poly);
    auto isConvex = [&](int i0, int i1, int i2) {
        const TINNode& A = poly[idx[i0]], & B = poly[idx[i1]], & C = poly[idx[i2]];
        const double cross = Cross2D(A.x, A.y, B.x, B.y, C.x, C.y);
        return ccw ? (cross > 0.0) : (cross < 0.0);
        };

    size_t guard = 0;
    while (idx.size() > 3 && guard++ < n * n) {
        bool clipped = false;
        for (size_t i = 0;i < idx.size();++i) {
            const int i0 = (int)((i + idx.size() - 1) % idx.size());
            const int i1 = (int)i;
            const int i2 = (int)((i + 1) % idx.size());
            if (!isConvex(i0, i1, i2)) continue;

            const TINNode& A = poly[idx[i0]], & B = poly[idx[i1]], & C = poly[idx[i2]];
            bool empty = true;
            for (size_t k = 0;k < idx.size();++k) {
                if (k == i0 || k == i1 || k == i2) continue;
                if (PointInTriStrictXY(poly[idx[k]].x, poly[idx[k]].y, A.x, A.y, B.x, B.y, C.x, C.y)) { empty = false; break; }
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
    constexpr double EPS = 1e-12;
    for (int ti = 0; ti < (int)tris.size(); ++ti) {
        const TINTri& t = tris[ti];
        const TINNode& A = nodes[t.a], & B = nodes[t.b], & C = nodes[t.c];
        const bool outside =
            (Cross2D(A.x, A.y, B.x, B.y, P.x, P.y) < -EPS) ||
            (Cross2D(B.x, B.y, C.x, C.y, P.x, P.y) < -EPS) ||
            (Cross2D(C.x, C.y, A.x, A.y, P.x, P.y) < -EPS);
        if (!outside) return ti;
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
// CDT legalization (constraints по границе)
// ================================================================
struct Edge { int u, v; };
static inline Edge MkE(int a, int b) { if (a > b) std::swap(a, b); return { a,b }; }
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
// Mesh base Z (Archicad 27): storyZ + mesh.level
// ================================================================
static double GetMeshBaseZ(const API_Element& meshElem)
{
    double storyZ = 0.0; GetStoryLevelZ(meshElem.header.floorInd, storyZ);
    const double baseZ = storyZ + meshElem.mesh.level;
    Log("[MeshBase] floor=%d storyZ=%.6f mesh.level=%.6f -> baseZ=%.6f",
        (int)meshElem.header.floorInd, storyZ, meshElem.mesh.level, baseZ);
    return baseZ;
}

// ================================================================
// Build contour nodes (outer) with absolute Z
// ================================================================
struct MeshPolyData { std::vector<TINNode> contour; bool ok = false; };

static MeshPolyData BuildContourNodes(const API_Element& elem, const API_ElementMemo& memo, double baseZ)
{
    MeshPolyData out{};
    if (memo.coords == nullptr || memo.meshPolyZ == nullptr || elem.mesh.poly.nCoords < 3) return out;

    const API_Coord* coords = *memo.coords;
    const Int32 nCoords = elem.mesh.poly.nCoords; // includes closing
    const bool coords1 = ((Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord)) == nCoords + 1);

    const double* zH = *memo.meshPolyZ;
    const Int32 zCnt = (Int32)(BMGetHandleSize((GSHandle)memo.meshPolyZ) / sizeof(double));
    const bool  z1 = (zCnt % (nCoords + 1) == 0);

    // Логируем первые несколько Z для отладки
    Log("[BuildContour] baseZ=%.6f nCoords=%d zCnt=%d coords1=%d z1=%d", baseZ, nCoords, zCnt, (int)coords1, (int)z1);
    for (Int32 i = 1; i <= std::min(nCoords, (Int32)5); ++i) {
        const API_Coord& c = coords[coords1 ? i : (i - 1)];
        const Int32 zi = z1 ? i : (i - 1);
        const double rawZ = zH[zi];
        const double absZ = baseZ + rawZ;
        Log("[BuildContour] i=%d XY=(%.3f,%.3f) rawZ=%.3f baseZ+rawZ=%.3f", i, c.x, c.y, rawZ, absZ);
    }

    out.contour.reserve((size_t)(nCoords - 1));
    for (Int32 i = 1; i <= nCoords - 1; ++i) {
        const API_Coord& c = coords[coords1 ? i : (i - 1)];
        const Int32 zi = z1 ? i : (i - 1);
        const double absZ = baseZ + zH[zi];
        // убираем только строго совпавшие подряд
        if (!out.contour.empty()) {
            const TINNode& last = out.contour.back();
            if (std::fabs(c.x - last.x) < 1e-12 && std::fabs(c.y - last.y) < 1e-12) continue;
        }
        out.contour.push_back({ c.x, c.y, absZ });
    }
    out.ok = true;
    
    return out;
}

// ================================================================
// Barycentric helpers
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

// ================================================================
// Ray casting (Möllер–Trumbore)
// ================================================================
static inline bool RayTriHit_MT(const API_Coord3D& orig, const API_Vector3D& dirUnit,
    const API_Coord3D& A, const API_Coord3D& B, const API_Coord3D& C,
    double& outT)
{
    const double EPS = 1e-12;
    API_Vector3D e1{ B.x - A.x, B.y - A.y, B.z - A.z };
    API_Vector3D e2{ C.x - A.x, C.y - A.y, C.z - A.z };

    API_Vector3D p{
        dirUnit.y * e2.z - dirUnit.z * e2.y,
        dirUnit.z * e2.x - dirUnit.x * e2.z,
        dirUnit.x * e2.y - dirUnit.y * e2.x
    };
    const double det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
    if (std::fabs(det) < EPS) return false;
    const double invDet = 1.0 / det;

    API_Vector3D tvec{ orig.x - A.x, orig.y - A.y, orig.z - A.z };
    const double u = (tvec.x * p.x + tvec.y * p.y + tvec.z * p.z) * invDet;
    if (u < -EPS || u > 1.0 + EPS) return false;

    API_Vector3D q{
        tvec.y * e1.z - tvec.z * e1.y,
        tvec.z * e1.x - tvec.x * e1.z,
        tvec.x * e1.y - tvec.y * e1.x
    };
    const double v = (dirUnit.x * q.x + dirUnit.y * q.y + dirUnit.z * q.z) * invDet;
    if (v < -EPS || u + v > 1.0 + EPS) return false;

    outT = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * invDet;
    return outT > EPS;
}

// ================================================================
// Build TIN from memo (contour + optional level points + CDT)
// ================================================================
static bool BuildTIN_FromMemo(const API_Element& elem, const API_ElementMemo& memo,
    std::vector<TINNode>& nodes, std::vector<TINTri>& tris,
    double& outBaseZ)
{
    nodes.clear(); tris.clear(); outBaseZ = 0.0;

    const double baseZ = GetMeshBaseZ(elem);
    outBaseZ = baseZ;

    // 1) Контур
    MeshPolyData mp = BuildContourNodes(elem, memo, baseZ);
    if (!mp.ok || mp.contour.size() < 3) { Log("[TIN] contour build failed"); return false; }

    nodes = mp.contour;
    Log("[TIN] Contour nodes=%u before triangulation", (unsigned)nodes.size());
    for (size_t i = 0; i < std::min(nodes.size(), (size_t)5); ++i) {
        Log("[TIN] node[%u]=(%.3f,%.3f,%.3f)", (unsigned)i, nodes[i].x, nodes[i].y, nodes[i].z);
    }
    
    tris = TriangulateEarClipping(nodes);
    if (tris.empty()) { Log("[TIN] triangulation failed"); return false; }
    Log("[TIN] Triangulated: %u tris", (unsigned)tris.size());

    // Запоминаем границу ДО вставки level-точек
    const int boundaryCount = (int)mp.contour.size();
    EdgeSet constraints;
    if (boundaryCount > 0) {
        for (int i = 0; i < boundaryCount; ++i) constraints.insert(MkE(i, (i + 1) % boundaryCount));
    }

    // 2) Level-точки (врезка)
    const int lvlCnt = memo.meshLevelCoords ? (int)(BMGetHandleSize((GSHandle)memo.meshLevelCoords) / sizeof(API_MeshLevelCoord)) : 0;
    Log("[TIN] Adding level points: %d available", lvlCnt);
    if (lvlCnt > 0) {
        const API_MeshLevelCoord* lvl = *memo.meshLevelCoords;
        const int maxN = std::min(lvlCnt, (int)MAX_LEVEL_POINTS);
        auto key = [](double x, double y) { return std::pair<long long, long long>{
            (long long)std::llround(x * 1e6), (long long)std::llround(y * 1e6)}; };
        
        // Сначала добавляем контурные узлы в seen
        std::set<std::pair<long long, long long>> seen;
        for (const auto& n : nodes) {
            seen.insert(key(n.x, n.y));
        }
        
        int inserted = 0;
        for (int i = 0; i < maxN; ++i) {
            if (!seen.insert(key(lvl[i].c.x, lvl[i].c.y)).second) continue;
            const TINNode P{ lvl[i].c.x, lvl[i].c.y, baseZ + lvl[i].c.z };
            const int triIdx = FindTriContaining(nodes, tris, P);
            if (triIdx >= 0) {
                const int pIdx = (int)nodes.size(); nodes.push_back(P);
                int t0, t1, t2; SplitTriByPoint(nodes, tris, triIdx, pIdx, t0, t1, t2);
                inserted++;
                if (inserted <= 5) Log("[TIN] inserted level[%d]: XY=(%.3f,%.3f) rawZ=%.3f absZ=%.3f", i, lvl[i].c.x, lvl[i].c.y, lvl[i].c.z, P.z);
            }
        }
        Log("[TIN] Inserted %d level points", inserted);
        if (lvlCnt > MAX_LEVEL_POINTS) Log("[TIN] level points truncated: %d -> %d", lvlCnt, (int)MAX_LEVEL_POINTS);
    }

    // 3) CDT по границе
    GlobalConstrainedDelaunayLegalize(nodes, tris, constraints);

    return !nodes.empty() && !tris.empty();
}

// ================================================================
// Sample Z at XY using TIN (barycentric, then vertical raycast)
// ================================================================
static bool SampleZ_OnTIN(const std::vector<TINNode>& nodes, const std::vector<TINTri>& tris,
    const API_Coord3D& posXY, double& outZ, API_Vector3D& outN)
{
    const TINNode P{ posXY.x, posXY.y, 0.0 };
    int triHit = FindTriContaining(nodes, tris, P);

    if (triHit >= 0) {
        const TINTri& t = tris[triHit];
        double wA, wB, wC; BaryXY(P, nodes[t.a], nodes[t.b], nodes[t.c], wA, wB, wC);
        outZ = wA * nodes[t.a].z + wB * nodes[t.b].z + wC * nodes[t.c].z;
        outN = TriNormal3D(nodes[t.a], nodes[t.b], nodes[t.c]);
        
        Log("[SampleZ] P=(%.3f,%.3f) -> tri[%d]=%d,%d,%d weights=(%.3f,%.3f,%.3f) Z=(%.3f,%.3f,%.3f) -> %.3f",
            P.x, P.y, triHit, t.a, t.b, t.c, wA, wB, wC,
            nodes[t.a].z, nodes[t.b].z, nodes[t.c].z, outZ);
        
        return true;
    }
    
    Log("[SampleZ] P=(%.3f,%.3f) -> NO TRIANGLE FOUND", P.x, P.y);

    // вертикальный луч вниз
    API_Coord3D orig{ posXY.x, posXY.y, 1e9 };
    API_Vector3D dir{ 0,0,-1 };
    double bestT = 1e100; int bestIdx = -1;
    for (int i = 0; i < (int)tris.size(); ++i) {
        const TINTri& t = tris[i];
        const API_Coord3D A{ nodes[t.a].x, nodes[t.a].y, nodes[t.a].z };
        const API_Coord3D B{ nodes[t.b].x, nodes[t.b].y, nodes[t.b].z };
        const API_Coord3D C{ nodes[t.c].x, nodes[t.c].y, nodes[t.c].z };
        double tp;
        if (RayTriHit_MT(orig, dir, A, B, C, tp)) {
            if (tp < bestT) { bestT = tp; bestIdx = i; }
        }
    }
    if (bestIdx >= 0) {
        outZ = orig.z + dir.z * bestT;
        const TINTri& bt = tris[bestIdx];
        outN = TriNormal3D(nodes[bt.a], nodes[bt.b], nodes[bt.c]);
        return true;
    }

    // фолбэк: ближайшая вершина
    double best = 1e12, bestZ = 0.0; int bestN = -1;
    for (int i = 0;i < (int)nodes.size();++i) {
        const double d = std::hypot(P.x - nodes[i].x, P.y - nodes[i].y);
        if (d < best) { best = d; bestZ = nodes[i].z; bestN = i; }
    }
    if (bestN >= 0) { outZ = bestZ; outN = { 0,0,1 }; return true; }
    return false;
}

// ================================================================
// Compute ground Z at point (mesh memo path)
// ================================================================
static bool ComputeGroundZ_MemoOnly(const API_Guid& meshGuid, const API_Coord3D& pos3D,
    double& outAbsZ, API_Vector3D& outNormal)
{
    outAbsZ = 0.0; outNormal = { 0,0,1 };

    // Проверяем кеш
    if (!g_hasCachedTIN || g_cachedMeshGuid != meshGuid) {
        Log("[TIN] Building TIN cache...");
        g_cachedNodes.clear();
        g_cachedTris.clear();
        g_cachedBaseZ = 0.0;
        
        API_Element elem{}; elem.header.guid = meshGuid;
        if (ACAPI_Element_Get(&elem) != NoError) { Log("[TIN] Element_Get(mesh) failed"); return false; }

        API_ElementMemo memo{};
        const GSErr mErr = ACAPI_Element_GetMemo(meshGuid, &memo,
            APIMemoMask_MeshLevel | APIMemoMask_Polygon | APIMemoMask_MeshPolyZ);
        if (mErr != NoError) { Log("[TIN] GetMemo failed err=%d", (int)mErr); return false; }

        const bool okTIN = BuildTIN_FromMemo(elem, memo, g_cachedNodes, g_cachedTris, g_cachedBaseZ);
        ACAPI_DisposeElemMemoHdls(&memo);
        if (!okTIN) { Log("[TIN] BuildTIN failed"); return false; }
        
        g_cachedMeshGuid = meshGuid;
        g_hasCachedTIN = true;
        Log("[TIN] Cache built: %u nodes, %u tris", (unsigned)g_cachedNodes.size(), (unsigned)g_cachedTris.size());
    }

    const bool okS = SampleZ_OnTIN(g_cachedNodes, g_cachedTris, pos3D, outAbsZ, outNormal);
    if (okS) {
        Log("[TIN] sample XY=(%.6f,%.6f) -> Z=%.6f  N=(%.4f,%.4f,%.4f)",
            pos3D.x, pos3D.y, outAbsZ, outNormal.x, outNormal.y, outNormal.z);
    }
    return okS;
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
    case LandableKind::Object: return { e.object.pos.x,      e.object.pos.y,      floorZ + e.object.level };
    case LandableKind::Lamp:   return { e.lamp.pos.x,        e.lamp.pos.y,        floorZ + e.lamp.level };
    case LandableKind::Column: return { e.column.origoPos.x, e.column.origoPos.y, floorZ + e.column.bottomOffset };
    default: return { 0,0,0 };
    }
}

// ФИКС: для колонны сохраняем высоту (двигаем и верх)
static void SetWorldZ_WithDelta(API_Element& e, double finalWorldZ, double deltaWorldZ, API_Element& maskOut)
{
    ACAPI_ELEMENT_MASK_CLEAR(maskOut);
    double floorZ = 0.0; GetStoryLevelZ(e.header.floorInd, floorZ);

    switch (IdentifyLandable(e)) {
    case LandableKind::Object: {
        const double old = e.object.level;
        e.object.level = finalWorldZ - floorZ;
        ACAPI_ELEMENT_MASK_SET(maskOut, API_ObjectType, level);
        Log("[SetZ:Object] old=%.6f new=%.6f (delta=%.6f)", old, e.object.level, deltaWorldZ);
        break;
    }
    case LandableKind::Lamp: {
        const double old = e.lamp.level;
        e.lamp.level = finalWorldZ - floorZ;
        ACAPI_ELEMENT_MASK_SET(maskOut, API_LampType, level);
        Log("[SetZ:Lamp] old=%.6f new=%.6f (delta=%.6f)", old, e.lamp.level, deltaWorldZ);
        break;
    }
    case LandableKind::Column: {
        const double oldBot = e.column.bottomOffset;
        const double oldTop = e.column.topOffset;
        e.column.bottomOffset = finalWorldZ - floorZ;
        e.column.topOffset = oldTop + deltaWorldZ; // сохранить высоту
        ACAPI_ELEMENT_MASK_SET(maskOut, API_ColumnType, bottomOffset);
        ACAPI_ELEMENT_MASK_SET(maskOut, API_ColumnType, topOffset);
        Log("[SetZ:Column] bottom %.6f->%.6f, topOffset %.6f->%.6f (delta=%.6f)",
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
    g_hasCachedTIN = false; // Сбрасываем кеш при смене поверхности

    API_SelectionInfo selInfo{}; GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    Log("[SetGroundSurface] neigs=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{}; el.header.guid = n.guid;
        const GSErr err = ACAPI_Element_Get(&el);
        Log("[SetGroundSurface] guid=%s typeID=%d err=%d mesh.level=%.6f",
            APIGuidToString(n.guid).ToCStr().Get(), (int)el.header.type.typeID, (int)err, el.mesh.level);
        if (err != NoError) continue;
        if (el.header.type.typeID == API_MeshID) {
            g_surfaceGuid = n.guid;
            Log("[SetGroundSurface] Mesh set: %s", APIGuidToString(n.guid).ToCStr().Get());
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
    return ComputeGroundZ_MemoOnly(g_surfaceGuid, pos3D, z, normal);
}

bool GroundHelper::ApplyGroundOffset(double offset /* meters */)
{
    Log("[ApplyGroundOffset] ENTER offset=%.6f", offset);
    if (g_surfaceGuid == APINULLGuid || g_objectGuids.IsEmpty()) { Log("[ApplyGroundOffset] no surface or no objects"); return false; }
    
    // Сбрасываем кеш TIN перед применением, чтобы использовать актуальные данные mesh
    g_hasCachedTIN = false;
    g_cachedMeshGuid = APINULLGuid;

    const GSErr cmdErr = ACAPI_CallUndoableCommand("Land to Mesh", [&]() -> GSErr {
        for (const API_Guid& guid : g_objectGuids) {
            API_Element e{}; if (!FetchElementByGuid(guid, e)) { Log("[Apply] fetch failed"); continue; }
            const LandableKind kind = IdentifyLandable(e); if (kind == LandableKind::Unsupported) { Log("[Apply] unsupported"); continue; }

            const API_Coord3D anchor = GetWorldAnchor(e);
            Log("[Apply] guid=%s kind=%d floor=%d anchor=(%.6f,%.6f,%.6f)",
                APIGuidToString(guid).ToCStr().Get(), (int)kind, (int)e.header.floorInd, anchor.x, anchor.y, anchor.z);

            double surfaceZ = 0.0; API_Vector3D n{ 0,0,1 };
            if (!ComputeGroundZ_MemoOnly(g_surfaceGuid, anchor, surfaceZ, n)) { Log("[Apply] can't sample Z — skip"); continue; }

            const double delta = surfaceZ - anchor.z + offset;
            const double finalZ = anchor.z + delta;
            Log("[Apply] surfaceZ=%.6f  anchorZ=%.6f  offset=%.6f delta=%.6f -> finalZ=%.6f  N=(%.3f,%.3f,%.3f)",
                surfaceZ, anchor.z, offset, delta, finalZ, n.x, n.y, n.z);

            API_Element mask{};
            SetWorldZ_WithDelta(e, finalZ, delta, mask);

            const GSErr chg = ACAPI_Element_Change(&e, &mask, nullptr, 0, true);
            if (chg == NoError) Log("[Apply] UPDATED %s", APIGuidToString(guid).ToCStr().Get());
            else               Log("[Apply] Change FAILED err=%d %s", (int)chg, APIGuidToString(guid).ToCStr().Get());
        }
        return NoError;
        });

    Log("[ApplyGroundOffset] EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

bool GroundHelper::ApplyZDelta(double deltaMeters)
{
    Log("[ApplyZDelta] ENTER delta=%.6f", deltaMeters);

    // ВСЕГДА обновляем кэш из текущего выделения, чтобы использовать актуальные объекты
    Log("[ApplyZDelta] updating cache from current selection");
    if (!SetGroundObjects()) { 
        Log("[ApplyZDelta] no objects in selection"); 
        return false; 
    }

    const GSErr cmdErr = ACAPI_CallUndoableCommand("Adjust Z by Delta", [=]() -> GSErr {
        for (const API_Guid& guid : g_objectGuids) {
            API_Element e{}; if (!FetchElementByGuid(guid, e)) { Log("[dltZ] fetch failed"); continue; }
            if (IdentifyLandable(e) == LandableKind::Unsupported) { Log("[dltZ] unsupported"); continue; }

            const API_Coord3D anchor = GetWorldAnchor(e);
            const double finalZ = anchor.z + deltaMeters;
            Log("[DeltaZ] guid=%s oldZ=%.6f -> newZ=%.6f (delta=%.6f)",
                APIGuidToString(guid).ToCStr().Get(), anchor.z, finalZ, deltaMeters);

            API_Element mask{};
            SetWorldZ_WithDelta(e, finalZ, deltaMeters, mask);

            const GSErr chg = ACAPI_Element_Change(&e, &mask, nullptr, 0, true);
            if (chg == NoError) Log("[DeltaZ] UPDATED %s", APIGuidToString(guid).ToCStr().Get());
            else               Log("[DeltaZ] Change FAILED err=%d %s", (int)chg, APIGuidToString(guid).ToCStr().Get());
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

