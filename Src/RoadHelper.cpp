#include "RoadHelper.hpp"

#include "BrowserRepl.hpp"
#include "GroundHelper.hpp"
#include "ShellHelper.hpp"

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "APICommon.h"
#include "APIdefs_Elements.h"
#include "APIdefs_3D.h"
#include "BM.hpp"

#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace RoadHelper {

    constexpr double kPI = 3.1415926535897932384626433832795;
    constexpr double kEPS = 1e-9;

    // ----------------------------------------------------------------------------
    // глобальные guid и этаж
    // ----------------------------------------------------------------------------
    static API_Guid g_centerLineGuid = APINULLGuid;
    static API_Guid g_terrainMeshGuid = APINULLGuid;
    static short    g_refFloor = 0;

    // ----------------------------------------------------------------------------
    // лог
    // ----------------------------------------------------------------------------
    static inline void Log(const char* fmt, ...)
    {
        va_list vl;
        va_start(vl, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, vl);
        va_end(vl);

        GS::UniString s(buf);

        if (BrowserRepl::HasInstance()) {
            BrowserRepl::GetInstance().LogToBrowser(s);
        }

        ACAPI_WriteReport("%s", false, s.ToCStr().Get());
    }

    // ----------------------------------------------------------------------------
    // выбрать осевую линию (пользователь сам выделил путь в Archicad)
    // ----------------------------------------------------------------------------
    bool SetCenterLine()
    {
        Log("[RoadHelper] Выбор осевой линии...");

        API_SelectionInfo   selInfo;
        GS::Array<API_Neig> selNeigs;
        if (ACAPI_Selection_Get(&selInfo, &selNeigs, false, false) != NoError || selNeigs.IsEmpty()) {
            Log("[RoadHelper] Нет выделения для осевой линии");
            g_centerLineGuid = APINULLGuid;
            return false;
        }

        g_centerLineGuid = selNeigs[0].guid;

        // забираем этаж
        API_Elem_Head head{};
        head.guid = g_centerLineGuid;
        if (ACAPI_Element_GetHeader(&head) == NoError) {
            g_refFloor = head.floorInd;
        }

        Log("[RoadHelper] Осевая линия зафиксирована: %s (floor=%d)",
            APIGuidToString(g_centerLineGuid).ToCStr().Get(),
            (int)g_refFloor);

        // подчистим выделение структуры выбора
        BMKillHandle((GSHandle*)&selInfo.marquee.coords);
        return true;
    }

    // ----------------------------------------------------------------------------
    // выбрать mesh рельефа (нужно просто чтобы не ругалось BuildRoad)
    // ----------------------------------------------------------------------------
    bool SetTerrainMesh()
    {
        Log("[RoadHelper] Выбор Mesh рельефа...");

        API_SelectionInfo   selInfo;
        GS::Array<API_Neig> selNeigs;
        if (ACAPI_Selection_Get(&selInfo, &selNeigs, false, false) != NoError || selNeigs.IsEmpty()) {
            Log("[RoadHelper] Нет выделения Mesh");
            g_terrainMeshGuid = APINULLGuid;
            return false;
        }

        // берём первый mesh из выделения
        for (const API_Neig& n : selNeigs) {
            API_Element hdr = {};
            hdr.header.guid = n.guid;
            if (ACAPI_Element_GetHeader(&hdr.header) == NoError &&
                hdr.header.type.typeID == API_MeshID)
            {
                g_terrainMeshGuid = n.guid;
                Log("[RoadHelper] Mesh рельефа: %s",
                    APIGuidToString(g_terrainMeshGuid).ToCStr().Get());
                BMKillHandle((GSHandle*)&selInfo.marquee.coords);
                return true;
            }
        }

        Log("[RoadHelper] В выделении нет Mesh");
        g_terrainMeshGuid = APINULLGuid;
        BMKillHandle((GSHandle*)&selInfo.marquee.coords);
        return false;
    }

    // ============================================================================
    // Вспомогательная геометрия 2D для одной осевой
    // ============================================================================

    // Структуры для работы с сегментами пути (скопировано из LandscapeHelper)
    struct Seg {
        enum Kind { Line, Arc } kind;
        API_Coord a{}, b{};   // Line
        API_Coord c{};        // Arc: center
        double    r = 0.0;
        double    a0 = 0.0;   // start angle
        double    a1 = 0.0;   // end angle (a1 - a0 = signed sweep)
        double    L = 0.0;   // length
    };

    // Вспомогательные функции для работы с сегментами
    static inline double Dist(const API_Coord& p, const API_Coord& q) {
        return std::hypot(q.x - p.x, q.y - p.y);
    }
    static inline API_Coord Lerp(const API_Coord& p, const API_Coord& q, double t) {
        return { p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t };
    }
    static inline void PushLine(std::vector<Seg>& segs, const API_Coord& a, const API_Coord& b) {
        Seg s; s.kind = Seg::Line; s.a = a; s.b = b; s.L = Dist(a, b);
        if (s.L > 1e-9) segs.push_back(s);
    }
    static inline double Norm2PI(double a) {
        const double two = 2.0 * kPI;
        while (a < 0.0)   a += two;
        while (a >= two)  a -= two;
        return a;
    }
    static inline double CCWDelta(double a0, double a1) {
        a0 = Norm2PI(a0); a1 = Norm2PI(a1);
        double d = a1 - a0; if (d < 0.0) d += 2.0 * kPI;
        return d; // [0,2pi)
    }

    // Безье для сплайна
    static inline API_Coord Add(const API_Coord& a, const API_Coord& b) { return { a.x + b.x, a.y + b.y }; }
    static inline API_Coord Sub(const API_Coord& a, const API_Coord& b) { return { a.x - b.x, a.y - b.y }; }
    static inline API_Coord Mul(const API_Coord& a, double s) { return { a.x * s,   a.y * s }; }
    static inline API_Coord FromAngLen(double ang, double len) { return { std::cos(ang) * len, std::sin(ang) * len }; }
    static inline API_Coord BezierPoint(const API_Coord& P0, const API_Coord& C1,
        const API_Coord& C2, const API_Coord& P3, double t)
    {
        const double u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        return { b0 * P0.x + b1 * C1.x + b2 * C2.x + b3 * P3.x,
                 b0 * P0.y + b1 * C1.y + b2 * C2.y + b3 * P3.y };
    }

    // Сборка сегментов пути из элемента
    static bool BuildPathSegments(const API_Guid& pathGuid, std::vector<Seg>& segs, double* totalLen)
    {
        segs.clear();
        if (totalLen) *totalLen = 0.0;

        API_Element e = {}; e.header.guid = pathGuid;
        if (ACAPI_Element_Get(&e) != NoError) return false;

        switch (e.header.type.typeID) {
        case API_LineID:
            PushLine(segs, e.line.begC, e.line.endC);
            break;

        case API_ArcID: {
            Seg s; s.kind = Seg::Arc; s.c = e.arc.origC; s.r = e.arc.r;
            double a0 = Norm2PI(e.arc.begAng);
            double sweep = e.arc.endAng - a0;
            while (sweep <= -2.0 * kPI) sweep += 2.0 * kPI;
            while (sweep > 2.0 * kPI) sweep -= 2.0 * kPI;
            s.a0 = a0; s.a1 = a0 + sweep; s.L = s.r * std::fabs(sweep);
            if (s.L > 1e-9) segs.push_back(s);
            break;
        }

        case API_CircleID: {
            Seg s; s.kind = Seg::Arc; s.c = e.circle.origC; s.r = e.circle.r;
            s.a0 = 0.0; s.a1 = 2.0 * kPI; s.L = 2.0 * kPI * s.r;
            segs.push_back(s);
            break;
        }

        case API_PolyLineID: {
            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo(pathGuid, &memo) == NoError && memo.coords != nullptr) {
                const Int32 nAll = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord));
                const Int32 nPts = std::max<Int32>(0, nAll - 1);
                if (nPts >= 2) {
                    for (Int32 i = 1; i <= nPts - 1; ++i) {
                        const API_Coord& A = (*memo.coords)[i];
                        const API_Coord& B = (*memo.coords)[i + 1];
                        PushLine(segs, A, B);
                    }
                }
            }
            ACAPI_DisposeElemMemoHdls(&memo);
            break;
        }

        case API_SplineID: {
            // Кубические Безье по bezierDirs
            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo(pathGuid, &memo, APIMemoMask_Polygon) == NoError &&
                memo.coords != nullptr && memo.bezierDirs != nullptr)
            {
                const Int32 n = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord));
                if (n >= 2) {
                    for (Int32 i = 0; i < n - 1; ++i) {
                        const API_Coord P0 = (*memo.coords)[i];
                        const API_Coord P3 = (*memo.coords)[i + 1];
                        const API_SplineDir d0 = (*memo.bezierDirs)[i];
                        const API_SplineDir d1 = (*memo.bezierDirs)[i + 1];
                        const API_Coord C1 = Add(P0, FromAngLen(d0.dirAng, d0.lenNext));
                        const API_Coord C2 = Sub(P3, FromAngLen(d1.dirAng, d1.lenPrev));

                        const int N = 32; // сабсегментов на ребро
                        API_Coord prev = P0;
                        for (int k = 1; k <= N; ++k) {
                            const double t = (double)k / (double)N;
                            const API_Coord pt = BezierPoint(P0, C1, C2, P3, t);
                            PushLine(segs, prev, pt);
                            prev = pt;
                        }
                    }
                }
            }
            ACAPI_DisposeElemMemoHdls(&memo);
            break;
        }

        default: return false;
        }

        if (segs.empty()) return false;

        double sum = 0.0; for (const Seg& s : segs) sum += s.L;
        if (totalLen) *totalLen = sum;

        Log("[RoadHelper] path len=%.3f, segs=%u", sum, (unsigned)segs.size());
        return sum > 1e-9;
    }

    // Параметризация по длине s
    static void EvalOnPath(const std::vector<Seg>& segs, double s, API_Coord* outP, double* outTanAngleRad)
    {
        double acc = 0.0;
        for (const Seg& seg : segs) {
            if (s > acc + seg.L) { acc += seg.L; continue; }
            const double f = (seg.L < 1e-9) ? 0.0 : (s - acc) / seg.L;

            if (seg.kind == Seg::Line) {
                if (outP)           *outP = Lerp(seg.a, seg.b, f);
                if (outTanAngleRad) *outTanAngleRad = std::atan2(seg.b.y - seg.a.y, seg.b.x - seg.a.x);
            }
            else {
                const double sweep = seg.a1 - seg.a0;                 // со знаком!
                const double ang = seg.a0 + f * sweep;
                if (outP)           *outP = { seg.c.x + seg.r * std::cos(ang), seg.c.y + seg.r * std::sin(ang) };
                if (outTanAngleRad) *outTanAngleRad = ang + ((sweep >= 0.0) ? +kPI / 2.0 : -kPI / 2.0);
            }
            return;
        }
        const Seg& seg = segs.back();
        if (seg.kind == Seg::Line) {
            if (outP)           *outP = seg.b;
            if (outTanAngleRad) *outTanAngleRad = std::atan2(seg.b.y - seg.a.y, seg.b.x - seg.a.x);
        }
        else {
            const double sweep = seg.a1 - seg.a0;
            if (outP)           *outP = { seg.c.x + seg.r * std::cos(seg.a1), seg.c.y + seg.r * std::sin(seg.a1) };
            if (outTanAngleRad) *outTanAngleRad = seg.a1 + ((sweep >= 0.0) ? +kPI / 2.0 : -kPI / 2.0);
        }
    }

    // 1) Собираем список XY-точек оси как ломаную
    //    Для дуг генерируем точки по окружности
    static bool CollectAxisPoints2D(const API_Guid& guid, GS::Array<API_Coord>& outPts)
    {
        outPts.Clear();

        API_Element el = {};
        el.header.guid = guid;
        if (ACAPI_Element_Get(&el) != NoError) {
            Log("[RoadHelper] CollectAxisPoints2D: не смогли прочитать элемент оси");
            return false;
        }

        switch (el.header.type.typeID) {
        case API_LineID: {
            outPts.Push(el.line.begC);
            outPts.Push(el.line.endC);
            break;
        }

        case API_ArcID: {
            // Для дуги генерируем точки по окружности
            const API_Coord center = el.arc.origC;
            const double radius = el.arc.r;
            const double startAngle = el.arc.begAng;
            const double endAngle = el.arc.endAng;
            
            // Вычисляем количество точек для дуги (минимум 8, максимум 64)
            double sweep = endAngle - startAngle;
            while (sweep <= -2.0 * kPI) sweep += 2.0 * kPI;
            while (sweep > 2.0 * kPI) sweep -= 2.0 * kPI;
            
            const double absSweep = std::abs(sweep);
            const int numPoints = std::max(8, std::min(64, (int)(absSweep * 16.0 / kPI)));
            
            for (int i = 0; i <= numPoints; ++i) {
                const double t = (double)i / (double)numPoints;
                const double angle = startAngle + t * sweep;
                const API_Coord pt = {
                    center.x + radius * std::cos(angle),
                    center.y + radius * std::sin(angle)
                };
                outPts.Push(pt);
            }
            
            Log("[RoadHelper] CollectAxisPoints2D: сгенерировано %u точек для дуги", (unsigned)outPts.GetSize());
            break;
        }

        case API_CircleID: {
            // Для круга генерируем точки по окружности
            const API_Coord center = el.circle.origC;
            const double radius = el.circle.r;
            
            const int numPoints = 32; // фиксированное количество точек для круга
            for (int i = 0; i < numPoints; ++i) {
                const double angle = 2.0 * kPI * (double)i / (double)numPoints;
                const API_Coord pt = {
                    center.x + radius * std::cos(angle),
                    center.y + radius * std::sin(angle)
                };
                outPts.Push(pt);
            }
            
            Log("[RoadHelper] CollectAxisPoints2D: сгенерировано %u точек для круга", (unsigned)outPts.GetSize());
            break;
        }

        case API_PolyLineID:
        case API_SplineID: {
            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_Polygon) == NoError &&
                memo.coords != nullptr)
            {
                const Int32 nAll = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / sizeof(API_Coord));
                const Int32 nPts = std::max<Int32>(0, nAll - 1);
                for (Int32 i = 1; i <= nPts; ++i) { // coords[0] сторож
                    outPts.Push((*memo.coords)[i]);
                }
            }
            ACAPI_DisposeElemMemoHdls(&memo);
            break;
        }

        default:
            Log("[RoadHelper] CollectAxisPoints2D: неподдерживаемый тип");
            return false;
        }

        if (outPts.GetSize() < 2) {
            Log("[RoadHelper] CollectAxisPoints2D: мало точек");
            outPts.Clear();
            return false;
        }

        return true;
    }

    // Проверка замкнутости линии (сравнение начальной и конечной точек с точностью 1мм)
    static bool IsLineClosed(const GS::Array<API_Coord>& pts)
    {
        if (pts.GetSize() < 3) return false; // минимум 3 точки для замкнутой линии
        
        const API_Coord& first = pts[0];
        const API_Coord& last = pts[pts.GetSize() - 1];
        
        const double tolerance = 1.0; // 1мм
        const double dx = last.x - first.x;
        const double dy = last.y - first.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        
        bool isClosed = (dist <= tolerance);
        Log("[RoadHelper] Проверка замкнутости: dist=%.3fмм, closed=%s", dist, isClosed ? "ДА" : "НЕТ");
        
        return isClosed;
    }

    // 2) Вычисляем нормаль в точке i по соседним сегментам
    static void ComputePerpAtIndex(
        const GS::Array<API_Coord>& axis,
        UIndex i,
        double& nx,
        double& ny
    ) {
        // Берём направление касательной как разницу между соседними точками
        UIndex i0 = (i == 0) ? 0 : i - 1;
        UIndex i1 = (i + 1 < axis.GetSize()) ? i + 1 : i;

        const API_Coord& A = axis[i0];
        const API_Coord& B = axis[i1];

        double vx = B.x - A.x;
        double vy = B.y - A.y;
        double len = std::sqrt(vx * vx + vy * vy);
        if (len < kEPS) {
            nx = 0.0;
            ny = 0.0;
            return;
        }

        // нормализованная касательная
        vx /= len;
        vy /= len;

        // влево от направления (перпендикуляр)
        nx = -vy;
        ny = vx;
    }

    // 3) Строим левую и правую кромку просто сдвигом по этому перпендикуляру
    static void OffsetSides(
        const GS::Array<API_Coord>& axis,
        double halfWidthM,
        GS::Array<API_Coord>& leftSide,
        GS::Array<API_Coord>& rightSide
    ) {
        leftSide.Clear();
        rightSide.Clear();

        for (UIndex i = 0; i < axis.GetSize(); ++i) {
            double nx = 0.0, ny = 0.0;
            ComputePerpAtIndex(axis, i, nx, ny);

            API_Coord L{ axis[i].x + nx * halfWidthM,
                          axis[i].y + ny * halfWidthM };
            API_Coord R{ axis[i].x - nx * halfWidthM,
                          axis[i].y - ny * halfWidthM };

            leftSide.Push(L);
            rightSide.Push(R);
        }
    }

    // ============================================================================
    // создание линий в модели
    // ============================================================================

    // Сплайн по списку 2D-точек
    static API_Guid CreateSplineFromPts(const GS::Array<API_Coord>& pts)
    {
        if (pts.GetSize() < 2)
            return APINULLGuid;

        // мы используем ShellHelper::CreateSplineFromPoints
        return ShellHelper::CreateSplineFromPoints(pts);
    }

    // Прямая линия между двумя точками
    static bool CreateSimpleLine2D(const API_Coord& a, const API_Coord& b, const char* tag)
    {
        API_Element lineEl = {};
        lineEl.header.type = API_LineID;
        if (ACAPI_Element_GetDefaults(&lineEl, nullptr) != NoError)
            return false;

        lineEl.header.floorInd = g_refFloor;
        lineEl.line.begC = a;
        lineEl.line.endC = b;

        const GSErrCode e = ACAPI_CallUndoableCommand("Create Road Line", [&]() -> GSErrCode {
            return ACAPI_Element_Create(&lineEl, nullptr);
            });

        if (e == NoError) {
            Log("[RoadHelper] Line created (%s)", tag);
            return true;
        }

        Log("[RoadHelper] Line FAILED (%s): err=%d", tag, (int)e);
        return false;
    }

    // ============================================================================
    // главная команда
    // ============================================================================

    // Функция откладывания точек по spline с заданным расстоянием
    static bool SamplePointsAlongSpline(const API_Guid& splineGuid, double stepMM, GS::Array<API_Coord>& outPts)
    {
        outPts.Clear();
        
        std::vector<Seg> segs;
        double totalLen = 0.0;
        
        if (!BuildPathSegments(splineGuid, segs, &totalLen)) {
            Log("[RoadHelper] ERROR: не удалось построить сегменты пути");
            return false;
        }
        
        if (totalLen < 1e-6) {
            Log("[RoadHelper] ERROR: путь слишком короткий");
            return false;
        }
        
        const double stepM = stepMM / 1000.0; // мм -> м
        const double epsilon = 1e-6;
        
        // Откладываем точки с заданным шагом
        for (double s = 0.0; s <= totalLen + epsilon; s += stepM) {
            const double clampedS = std::min(s, totalLen);
            API_Coord pt;
            EvalOnPath(segs, clampedS, &pt, nullptr);
            outPts.Push(pt);
        }
        
        // Обязательно добавляем последнюю точку
        API_Coord lastPt;
        EvalOnPath(segs, totalLen, &lastPt, nullptr);
        outPts.Push(lastPt);
        
        Log("[RoadHelper] Отложено %u точек по spline (шаг=%.1fмм, длина=%.3fм)", 
            (unsigned)outPts.GetSize(), stepMM, totalLen);
        
        return outPts.GetSize() >= 2;
    }

    // Копирование элемента с перемещением по вектору используя ACAPI_Element_Edit
    static API_Guid CopyElementWithOffset(const API_Guid& sourceGuid, double offsetX, double offsetY)
    {
        // Получаем информацию об элементе для определения правильного neigID
        API_Element sourceEl = {};
        sourceEl.header.guid = sourceGuid;
        if (ACAPI_Element_Get(&sourceEl) != NoError) {
            Log("[RoadHelper] ERROR: не удалось прочитать исходный элемент");
            return APINULLGuid;
        }

        // Создаем массив с одним элементом для редактирования
        GS::Array<API_Neig> items;
        API_Neig neig;
        neig.guid = sourceGuid;
        
        // Определяем правильный neigID в зависимости от типа элемента
        switch (sourceEl.header.type.typeID) {
        case API_LineID:
            neig.neigID = APINeig_Line;
            break;
        case API_ArcID:
            neig.neigID = APINeig_Arc;
            break;
        case API_CircleID:
            neig.neigID = APINeig_Circ;
            break;
        case API_PolyLineID:
            neig.neigID = APINeig_PolyLine;
            break;
        case API_SplineID:
            neig.neigID = APINeig_Spline;
            break;
        default:
            Log("[RoadHelper] ERROR: неподдерживаемый тип элемента для копирования");
            return APINULLGuid;
        }
        
        items.Push(neig);

        // Параметры редактирования для копирования с перемещением
        API_EditPars editPars = {};
        editPars.typeID = APIEdit_Drag;
        editPars.withDelete = false; // не удаляем оригинал
        editPars.begC.x = 0.0;
        editPars.begC.y = 0.0;
        editPars.begC.z = 0.0;
        editPars.endC.x = offsetX;
        editPars.endC.y = offsetY;
        editPars.endC.z = 0.0;

        // Выполняем редактирование (копирование с перемещением)
        const GSErrCode err = ACAPI_Element_Edit(&items, editPars);
        if (err == NoError && !items.IsEmpty()) {
            API_Guid newGuid = items[0].guid;
            Log("[RoadHelper] Копия создана: %s", APIGuidToString(newGuid).ToCStr().Get());
            return newGuid;
        } else {
            Log("[RoadHelper] ERROR: не удалось создать копию через Edit, err=%d", (int)err);
            return APINULLGuid;
        }
    }

    // Универсальный алгоритм для всех типов линий кроме spline
    static bool BuildUniversalRoad(const API_Guid& sourceGuid, double halfWidthM, API_Guid& leftGuid, API_Guid& rightGuid)
    {
        // Получаем точки исходной линии для определения направления
        GS::Array<API_Coord> centerPts;
        if (!CollectAxisPoints2D(sourceGuid, centerPts)) {
            Log("[RoadHelper] ERROR: не удалось прочитать точки линии");
            return false;
        }

        if (centerPts.GetSize() < 2) {
            Log("[RoadHelper] ERROR: недостаточно точек для универсального алгоритма");
            return false;
        }

        // Проверяем замкнутость
        bool isClosed = IsLineClosed(centerPts);
        Log("[RoadHelper] Универсальный алгоритм: %s линия, точек=%u", 
            isClosed ? "замкнутая" : "открытая", (unsigned)centerPts.GetSize());

        // Для дуг и кругов используем специальный алгоритм с построением перпендикуляров
        API_Element el = {};
        el.header.guid = sourceGuid;
        if (ACAPI_Element_Get(&el) == NoError && 
            (el.header.type.typeID == API_ArcID || el.header.type.typeID == API_CircleID)) {
            
            // Используем алгоритм с перпендикулярами для дуг
            GS::Array<API_Coord> leftPts, rightPts;
            if (!BuildPerpendicularPoints(centerPts, halfWidthM, leftPts, rightPts)) {
                Log("[RoadHelper] ERROR: не удалось построить перпендикуляры для дуги");
                return false;
            }
            
            // Создаем сплайны из точек
            leftGuid = CreateSplineFromPts(leftPts);
            rightGuid = CreateSplineFromPts(rightPts);
            
            if (leftGuid == APINULLGuid || rightGuid == APINULLGuid) {
                Log("[RoadHelper] ERROR: не удалось создать сплайны для дуги");
                return false;
            }
            
            Log("[RoadHelper] Универсальный алгоритм: созданы сплайны для дуги L=%s R=%s (ширина=%.3fм)", 
                APIGuidToString(leftGuid).ToCStr().Get(),
                APIGuidToString(rightGuid).ToCStr().Get(),
                halfWidthM * 2.0);
            
            return true;
        }

        // Для остальных типов линий используем копирование с смещением
        // Определяем направление перпендикуляра
        double nx = 0.0, ny = 0.0;
        
        if (isClosed) {
            // Для замкнутых линий используем направление от первой точки
            ComputePerpAtIndex(centerPts, 0, nx, ny);
        } else {
            // Для открытых линий используем направление от начала к концу
            const API_Coord& start = centerPts[0];
            const API_Coord& end = centerPts[centerPts.GetSize() - 1];
            
            double dx = end.x - start.x;
            double dy = end.y - start.y;
            double len = std::sqrt(dx * dx + dy * dy);
            
            if (len < kEPS) {
                Log("[RoadHelper] ERROR: линия слишком короткая");
                return false;
            }
            
            // Перпендикуляр (поворот на 90 градусов)
            nx = -dy / len;
            ny = dx / len;
        }

        // Создаем две копии с смещением по перпендикуляру в undoable команде
        const double leftOffsetX = nx * halfWidthM;
        const double leftOffsetY = ny * halfWidthM;
        const double rightOffsetX = -nx * halfWidthM;
        const double rightOffsetY = -ny * halfWidthM;

        GSErrCode err = ACAPI_CallUndoableCommand("Copy Road Lines", [&]() -> GSErrCode {
            leftGuid = CopyElementWithOffset(sourceGuid, leftOffsetX, leftOffsetY);
            rightGuid = CopyElementWithOffset(sourceGuid, rightOffsetX, rightOffsetY);
            
            if (leftGuid == APINULLGuid || rightGuid == APINULLGuid) {
                Log("[RoadHelper] ERROR: не удалось создать копии линии");
                return APIERR_GENERAL;
            }
            
            return NoError;
        });

        if (err != NoError) {
            Log("[RoadHelper] ERROR: не удалось выполнить команду копирования, err=%d", (int)err);
            return false;
        }

        Log("[RoadHelper] Универсальный алгоритм: созданы копии L=%s R=%s (ширина=%.3fм)", 
            APIGuidToString(leftGuid).ToCStr().Get(),
            APIGuidToString(rightGuid).ToCStr().Get(),
            halfWidthM * 2.0);

        return true;
    }

    // Функция построения перпендикуляров для получения двух рядов точек
    bool BuildPerpendicularPoints(const GS::Array<API_Coord>& centerPts, double halfWidthM, 
                                 GS::Array<API_Coord>& leftPts, GS::Array<API_Coord>& rightPts)
    {
        leftPts.Clear();
        rightPts.Clear();
        
        if (centerPts.GetSize() < 2) {
            Log("[RoadHelper] ERROR: недостаточно точек для построения перпендикуляров");
            return false;
        }
        
        for (UIndex i = 0; i < centerPts.GetSize(); ++i) {
            double nx = 0.0, ny = 0.0;
            ComputePerpAtIndex(centerPts, i, nx, ny);
            
            const API_Coord& center = centerPts[i];
            API_Coord left{ center.x + nx * halfWidthM, center.y + ny * halfWidthM };
            API_Coord right{ center.x - nx * halfWidthM, center.y - ny * halfWidthM };
            
            leftPts.Push(left);
            rightPts.Push(right);
        }
        
        Log("[RoadHelper] Построено %u перпендикуляров (ширина=%.3fм)", 
            (unsigned)leftPts.GetSize(), halfWidthM * 2.0);
        
        return leftPts.GetSize() >= 2 && rightPts.GetSize() >= 2;
    }

    bool BuildRoad(const RoadParams& params)
    {
        Log("[RoadHelper] >>> BuildRoad: width=%.1fмм, step=%.1fмм",
            params.widthMM, params.sampleStepMM);

        if (g_centerLineGuid == APINULLGuid) {
            Log("[RoadHelper] ERROR: нет осевой линии (сначала SetCenterLine())");
            return false;
        }
        if (g_refFloor == 0) {
            // не критично, но на всякий случай чтоб линии не улетели в другой этаж
            Log("[RoadHelper] WARN: g_refFloor=0");
        }

        if (params.widthMM <= 0.0) {
            Log("[RoadHelper] ERROR: ширина <= 0");
            return false;
        }

        // Определяем тип линии
        API_Element el = {};
        el.header.guid = g_centerLineGuid;
        if (ACAPI_Element_Get(&el) != NoError) {
            Log("[RoadHelper] ERROR: не удалось прочитать элемент");
            return false;
        }

        const double halfWidthM = (params.widthMM / 1000.0) * 0.5; // мм -> м/2
        GS::Array<API_Coord> leftPts;
        GS::Array<API_Coord> rightPts;
        bool success = false;

        // Выбираем алгоритм в зависимости от типа линии
        API_Guid leftGuid = APINULLGuid;
        API_Guid rightGuid = APINULLGuid;
        
        if (el.header.type.typeID == API_SplineID) {
            // Для spline используем старый алгоритм с откладыванием точек
            if (params.sampleStepMM <= 0.0) {
                Log("[RoadHelper] ERROR: для spline нужен шаг > 0");
                return false;
            }

            GS::Array<API_Coord> centerPts;
            if (!SamplePointsAlongSpline(g_centerLineGuid, params.sampleStepMM, centerPts)) {
                Log("[RoadHelper] ERROR: не удалось отложить точки по spline");
                return false;
            }

            GS::Array<API_Coord> leftPts, rightPts;
            success = BuildPerpendicularPoints(centerPts, halfWidthM, leftPts, rightPts);
            if (success) {
                leftGuid = CreateSplineFromPts(leftPts);
                rightGuid = CreateSplineFromPts(rightPts);
            }
            Log("[RoadHelper] Использован алгоритм для spline");
        } else {
            // Для всех остальных типов линий используем универсальный алгоритм с копированием
            success = BuildUniversalRoad(g_centerLineGuid, halfWidthM, leftGuid, rightGuid);
            Log("[RoadHelper] Использован универсальный алгоритм с копированием");
        }

        if (!success || leftGuid == APINULLGuid || rightGuid == APINULLGuid) {
            Log("[RoadHelper] ERROR: не удалось создать боковые линии");
            return false;
        }

        Log("[RoadHelper] боковые линии ок: L=%s  R=%s",
            APIGuidToString(leftGuid).ToCStr().Get(),
            APIGuidToString(rightGuid).ToCStr().Get());

        // Замыкаем начало и концы прямыми линиями (только для открытых линий)
        // Получаем точки для проверки замкнутости
        GS::Array<API_Coord> centerPts;
        if (CollectAxisPoints2D(g_centerLineGuid, centerPts)) {
            bool isClosed = IsLineClosed(centerPts);
            if (!isClosed) {
                // Для открытых линий создаем замыкающие линии
                // Получаем точки боковых линий
                GS::Array<API_Coord> leftPts, rightPts;
                if (CollectAxisPoints2D(leftGuid, leftPts) && CollectAxisPoints2D(rightGuid, rightPts)) {
                    if (leftPts.GetSize() >= 2 && rightPts.GetSize() >= 2) {
                        const UIndex last = leftPts.GetSize() - 1;
                        const API_Coord capA0 = leftPts[0];
                        const API_Coord capB0 = rightPts[0];
                        const API_Coord capA1 = leftPts[last];
                        const API_Coord capB1 = rightPts[last];

                        bool ok1 = CreateSimpleLine2D(capA0, capB0, "start cap");
                        bool ok2 = CreateSimpleLine2D(capA1, capB1, "end cap");

                        if (!ok1 || !ok2) {
                            Log("[RoadHelper] WARNING: не смогли сделать капы");
                        }
                    }
                }
            } else {
                Log("[RoadHelper] Замкнутая линия - капы не нужны");
            }
        }

        Log("[RoadHelper] ✅ ГОТОВО: создали контур дороги");
        return true;
    }

} // namespace RoadHelper
