#include "ACAPinc.h"
#include "APIEnvir.h"
#include "APICommon.h"
#include "APIdefs_3D.h"
#include "APIdefs_Elements.h"
#include "APIdefs_Goodies.h"
#include "ShellHelper.hpp"
#include "LandscapeHelper.hpp"
#include "GroundHelper.hpp"
#include "BrowserRepl.hpp"
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <limits>
#include <unordered_map>

static const double kEPS = 1e-9;
static constexpr double kPI = 3.14159265358979323846;

namespace ShellHelper {

// =============== Глобальные переменные ===============
API_Guid g_baseLineGuid = APINULLGuid;      // GUID базовой линии
API_Guid g_meshSurfaceGuid = APINULLGuid;   // GUID Mesh поверхности

// =============== Forward declarations ===============
API_Guid Create3DShell(const GS::Array<API_Coord3D>& points);

// =============== Логирование ===============
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

// =============== Выбор базовой линии ===============
bool SetBaseLineForShell()
{
    Log("[ShellHelper] SetBaseLineForShell: выбор базовой линии");
    
    // Используем существующую функцию LandscapeHelper::SetDistributionLine()
    // которая уже умеет выбирать линии/дуги/полилинии/сплайны
    bool success = LandscapeHelper::SetDistributionLine();
    
    if (success) {
        // Получаем GUID выбранной линии из выделения
        API_SelectionInfo selectionInfo;
        GS::Array<API_Neig> selNeigs;
        GSErrCode err = ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
        
        if (err == NoError && selNeigs.GetSize() > 0) {
            g_baseLineGuid = selNeigs[0].guid;
            Log("[ShellHelper] Базовая линия выбрана успешно, GUID: %s", 
                APIGuidToString(g_baseLineGuid).ToCStr().Get());
        } else {
            Log("[ShellHelper] Ошибка получения GUID выбранной линии");
            g_baseLineGuid = APINULLGuid;
        }
    } else {
        Log("[ShellHelper] Ошибка выбора базовой линии");
        g_baseLineGuid = APINULLGuid;
    }
    
    return success;
}

// =============== Основная функция создания оболочки ===============
bool CreateShellFromLine(double widthMM, double stepMM)
{
    Log("[ShellHelper] CreateShellFromLine: START, width=%.1fmm, step=%.1fmm", widthMM, stepMM);
    
    // Проверяем, что базовая линия и Mesh выбраны
    if (g_baseLineGuid == APINULLGuid) {
        Log("[ShellHelper] ERROR: Базовая линия не выбрана. Сначала выберите базовую линию.");
        return false;
    }
    
    if (g_meshSurfaceGuid == APINULLGuid) {
        Log("[ShellHelper] ERROR: Mesh поверхность не выбрана. Сначала выберите Mesh поверхность.");
        return false;
    }
    
    Log("[ShellHelper] Базовая линия: %s", APIGuidToString(g_baseLineGuid).ToCStr().Get());
    Log("[ShellHelper] Mesh поверхность: %s", APIGuidToString(g_meshSurfaceGuid).ToCStr().Get());
    
    // Получаем элемент базовой линии по сохраненному GUID
    API_Elem_Head elemHead = {};
    elemHead.guid = g_baseLineGuid;
    GSErrCode err = ACAPI_Element_GetHeader(&elemHead);
    
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось получить заголовок элемента базовой линии");
        return false;
    }
    
    // Проверяем поддерживаемые типы элементов
    bool isSupported = false;
    if (elemHead.type == API_LineID ||
        elemHead.type == API_PolyLineID ||
        elemHead.type == API_ArcID ||
        elemHead.type == API_CircleID ||
        elemHead.type == API_SplineID) {
        isSupported = true;
    }
    
    if (!isSupported) {
        Log("[ShellHelper] ERROR: Неподдерживаемый тип элемента базовой линии");
        Log("[ShellHelper] Поддерживаются: Line, Polyline, Arc, Circle, Spline");
        return false;
    }
    
    // Получаем данные элемента
    API_Element element = {};
    element.header = elemHead;
    err = ACAPI_Element_Get(&element);
    
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось получить данные элемента базовой линии");
        return false;
    }
    
    Log("[ShellHelper] Элемент базовой линии загружен успешно");
    
    // Парсим элемент в сегменты
    PathData path;
    if (!ParseElementToSegments(element, path)) {
        Log("[ShellHelper] ERROR: Не удалось распарсить элемент в сегменты");
        return false;
    }
    
    Log("[ShellHelper] Элемент распарсен: %d сегментов, общая длина %.3fм", 
        (int)path.segs.GetSize(), path.total);
    
    // Создаем настоящую 3D оболочку через Ruled Shell
    Log("[ShellHelper] Создаем 3D оболочку через Ruled Shell");
    return Create3DShellFromPath(path, widthMM, stepMM);
}

// =============== Анализ базовой линии ===============
GS::Array<API_Coord3D> AnalyzeBaseLine(const API_Guid& lineGuid, double stepMM)
{
    ACAPI_WriteReport("[ShellHelper] AnalyzeBaseLine: step=%.1fmm (заглушка)", false, stepMM);
    
    // TODO: Использовать существующие функции LandscapeHelper для получения точек линии
    // Пока возвращаем пустой массив
    GS::Array<API_Coord3D> points;
    return points;
}

// =============== Генерация перпендикулярных линий ===============
GS::Array<API_Coord3D> GeneratePerpendicularLines(
    const GS::Array<API_Coord3D>& basePoints, 
    double widthMM)
{
    ACAPI_WriteReport("[ShellHelper] GeneratePerpendicularLines: %d точек, ширина=%.1fmm", false, 
        (int)basePoints.GetSize(), widthMM);
    
    GS::Array<API_Coord3D> perpendicularPoints;
    
    if (basePoints.GetSize() < 2) {
        ACAPI_WriteReport("[ShellHelper] Недостаточно точек для генерации перпендикуляров", false);
        return perpendicularPoints;
    }
    
    // Проходим по базовым точкам и создаем перпендикуляры
    for (UIndex i = 0; i < basePoints.GetSize() - 1; ++i) {
        const API_Coord3D& current = basePoints[i];
        const API_Coord3D& next = basePoints[i + 1];
        
        // Вычисляем направление базовой линии
        double dx = next.x - current.x;
        double dy = next.y - current.y;
        double length = sqrt(dx * dx + dy * dy);
        
        if (length < 1e-6) continue; // Пропускаем слишком короткие сегменты
        
        // Нормализуем направление
        dx /= length;
        dy /= length;
        
        // Вычисляем перпендикулярное направление (поворот на 90° в плоскости XY)
        double perpX = -dy;
        double perpY = dx;
        
        // Создаем две точки перпендикуляра
        double halfWidth = widthMM / 2000.0; // Переводим мм в метры и делим пополам
        
        API_Coord3D left = {};
        left.x = current.x + perpX * halfWidth;
        left.y = current.y + perpY * halfWidth;
        left.z = current.z;
        perpendicularPoints.Push(left);
        
        API_Coord3D right = {};
        right.x = current.x - perpX * halfWidth;
        right.y = current.y - perpY * halfWidth;
        right.z = current.z;
        perpendicularPoints.Push(right);
    }
    
    ACAPI_WriteReport("[ShellHelper] Сгенерировано %d перпендикулярных точек", false, (int)perpendicularPoints.GetSize());
    return perpendicularPoints;
}

// =============== Проекция на 3D сетку ===============
GS::Array<API_Coord3D> ProjectToMesh(const GS::Array<API_Coord3D>& points)
{
    ACAPI_WriteReport("[ShellHelper] ProjectToMesh: %d точек", false, (int)points.GetSize());
    
    GS::Array<API_Coord3D> projectedPoints;
    
    for (const API_Coord3D& point : points) {
        // Используем GroundHelper::GetGroundZAndNormal для проекции точки на mesh
        API_Coord3D projected = point;
        
        double z = 0.0;
        API_Vector3D normal = {};
        if (GroundHelper::GetGroundZAndNormal(point, z, normal)) {
            projected.z = z;
            ACAPI_WriteReport("[ShellHelper] Точка (%.3f, %.3f) спроецирована на Z=%.3f", false, 
                point.x, point.y, z);
        } else {
            ACAPI_WriteReport("[ShellHelper] Не удалось спроецировать точку (%.3f, %.3f)", false, 
                point.x, point.y);
        }
        
        projectedPoints.Push(projected);
    }
    
    ACAPI_WriteReport("[ShellHelper] Спроецировано %d точек", false, (int)projectedPoints.GetSize());
    return projectedPoints;
}

// =============== Создание перпендикулярных линий ===============
bool CreatePerpendicularLines(const API_Element& baseLine, double widthMM)
{
    Log("[ShellHelper] CreatePerpendicularLines: width=%.1fmm", widthMM);
    
    // Вычисляем направление базовой линии
    API_Coord begC = baseLine.line.begC;
    API_Coord endC = baseLine.line.endC;
    
    double dx = endC.x - begC.x;
    double dy = endC.y - begC.y;
    double length = sqrt(dx * dx + dy * dy);
    
    if (length < 1e-6) {
        Log("[ShellHelper] ERROR: Базовая линия слишком короткая");
        return false;
    }
    
    // Нормализуем направление
    dx /= length;
    dy /= length;
    
    // Вычисляем перпендикулярное направление (поворот на 90°)
    double perpX = -dy;
    double perpY = dx;
    
    // Вычисляем смещение в метрах (widthMM в миллиметрах)
    double halfWidth = widthMM / 2000.0; // Переводим мм в метры и делим пополам
    
    Log("[ShellHelper] Направление линии: (%.3f, %.3f), перпендикуляр: (%.3f, %.3f), смещение: %.3fм", 
        dx, dy, perpX, perpY, halfWidth);
    
    // Создаем две перпендикулярные линии
    ACAPI_CallUndoableCommand("Create Shell Lines", [&] () -> GSErrCode {
        
        // Левая линия
        API_Element leftLine = {};
        leftLine.header.type = API_LineID;
        GSErrCode err = ACAPI_Element_GetDefaults(&leftLine, nullptr);
        if (err != NoError) {
            Log("[ShellHelper] ERROR: Не удалось получить настройки по умолчанию для левой линии");
            return err;
        }
        
        leftLine.header.floorInd = baseLine.header.floorInd;
        leftLine.line.begC.x = begC.x + perpX * halfWidth;
        leftLine.line.begC.y = begC.y + perpY * halfWidth;
        leftLine.line.endC.x = endC.x + perpX * halfWidth;
        leftLine.line.endC.y = endC.y + perpY * halfWidth;
        
        Log("[ShellHelper] Левая линия: begC=(%.3f,%.3f), endC=(%.3f,%.3f)", 
            leftLine.line.begC.x, leftLine.line.begC.y, 
            leftLine.line.endC.x, leftLine.line.endC.y);
        
        err = ACAPI_Element_Create(&leftLine, nullptr);
        if (err != NoError) {
            Log("[ShellHelper] ERROR: Не удалось создать левую линию, err=%d", (int)err);
            return err;
        }
        
        // Правая линия
        API_Element rightLine = {};
        rightLine.header.type = API_LineID;
        err = ACAPI_Element_GetDefaults(&rightLine, nullptr);
        if (err != NoError) {
            Log("[ShellHelper] ERROR: Не удалось получить настройки по умолчанию для правой линии");
            return err;
        }
        
        rightLine.header.floorInd = baseLine.header.floorInd;
        rightLine.line.begC.x = begC.x - perpX * halfWidth;
        rightLine.line.begC.y = begC.y - perpY * halfWidth;
        rightLine.line.endC.x = endC.x - perpX * halfWidth;
        rightLine.line.endC.y = endC.y - perpY * halfWidth;
        
        Log("[ShellHelper] Правая линия: begC=(%.3f,%.3f), endC=(%.3f,%.3f)", 
            rightLine.line.begC.x, rightLine.line.begC.y, 
            rightLine.line.endC.x, rightLine.line.endC.y);
        
        err = ACAPI_Element_Create(&rightLine, nullptr);
        if (err != NoError) {
            Log("[ShellHelper] ERROR: Не удалось создать правую линию, err=%d", (int)err);
            return err;
        }
        
        Log("[ShellHelper] SUCCESS: Обе перпендикулярные линии созданы");
        return NoError;
    });
    
    return true;
}

// =============== Создание геометрии оболочки ===============
bool CreateShellGeometry(const GS::Array<API_Coord3D>& shellPoints)
{
    ACAPI_WriteReport("[ShellHelper] CreateShellGeometry: %d точек", false, (int)shellPoints.GetSize());
    
    if (shellPoints.GetSize() < 2) {
        ACAPI_WriteReport("[ShellHelper] Недостаточно точек для создания оболочки", false);
        return false;
    }
    
    // TODO: Создать полилинию или другие элементы из точек оболочки
    // Пока просто логируем точки
    for (UIndex i = 0; i < shellPoints.GetSize(); ++i) {
        const API_Coord3D& point = shellPoints[i];
        ACAPI_WriteReport("[ShellHelper] Точка %d: (%.3f, %.3f, %.3f)", false, 
            (int)i, point.x, point.y, point.z);
    }
    
    ACAPI_WriteReport("[ShellHelper] Оболочка создана (пока заглушка)", false);
    return true;
}

// =============== Утилиты для работы с геометрией ===============
static inline double SegLenLine(const API_Coord& a, const API_Coord& b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

static inline double SegAng(const API_Coord& a, const API_Coord& b) { 
    return std::atan2(b.y - a.y, b.x - a.x); 
}

static inline bool NearlyEq(const API_Coord& a, const API_Coord& b, double tol = 1e-9) {
    return (std::fabs(a.x - b.x) <= tol) && (std::fabs(a.y - b.y) <= tol);
}

static inline API_Coord Add(const API_Coord& a, const API_Coord& b) { 
    return { a.x + b.x, a.y + b.y }; 
}

static inline API_Coord Sub(const API_Coord& a, const API_Coord& b) { 
    return { a.x - b.x, a.y - b.y }; 
}

static inline API_Coord Mul(const API_Coord& a, double s) { 
    return { a.x * s, a.y * s }; 
}

static inline API_Coord UnitFromAng(double ang) { 
    return { std::cos(ang), std::sin(ang) }; 
}

static inline double Clamp01(double t) { 
    return t < 0 ? 0 : (t > 1 ? 1 : t); 
}

static inline double NormPos(double a) { // в [0..2π)
    while (a < 0.0)       a += 2.0 * kPI;
    while (a >= 2.0 * kPI)  a -= 2.0 * kPI;
    return a;
}

// =============== Восстановление дуги по хорде и углу ===============
static bool BuildArcFromPolylineSegment(
    const API_Coord& A, const API_Coord& B, double arcAngle,
    API_Coord& C, double& r, double& a0, double& a1, bool& ccw)
{
    const double L = std::hypot(B.x - A.x, B.y - A.y);
    if (L <= kEPS || !std::isfinite(arcAngle))
        return false;

    // Нормируем угол в (-π, π]
    double phi = arcAngle;
    while (phi <= -kPI) phi += 2.0 * kPI;
    while (phi > kPI)  phi -= 2.0 * kPI;

    if (std::fabs(phi) < 1e-9)
        return false; // фактически прямая

    // Вычисляем радиус по minor-углу
    double rMinor = (0.5 * L) / std::sin(0.5 * phi);

    // Проверка на major-дугу
    bool isMajor = std::fabs(phi) > kPI;
    if (isMajor) {
        phi = phi > 0 ? phi - 2.0 * kPI : phi + 2.0 * kPI;
        r = (0.5 * L) / std::sin(0.5 * phi);
    } else {
        r = rMinor;
    }

    // Центр дуги
    const double midX = 0.5 * (A.x + B.x);
    const double midY = 0.5 * (A.y + B.y);
    const double perpLen = std::sqrt(r * r - (0.5 * L) * (0.5 * L));
    const double perpAng = SegAng(A, B) + (phi > 0 ? kPI / 2.0 : -kPI / 2.0);

    C = { midX + perpLen * std::cos(perpAng), midY + perpLen * std::sin(perpAng) };

    // Углы
    a0 = std::atan2(A.y - C.y, A.x - C.x);
    a1 = std::atan2(B.y - C.y, B.x - C.x);
    ccw = phi > 0;

    return true;
}

// =============== Парсинг элемента в сегменты ===============
bool ParseElementToSegments(const API_Element& element, PathData& path)
{
    path.segs.Clear();
    path.total = 0.0;
    
    if (element.header.type == API_LineID) {
        Seg seg;
        seg.type = SegType::Line;
        seg.A = element.line.begC;
        seg.B = element.line.endC;
        seg.len = SegLenLine(seg.A, seg.B);
        if (seg.len > kEPS) {
            path.segs.Push(seg);
            path.total += seg.len;
        }
        Log("[ShellHelper] Line parsed: length=%.3f", seg.len);
        return !path.segs.IsEmpty();
    }
    else if (element.header.type == API_ArcID) {
        Seg seg;
        seg.type = SegType::Arc;
        seg.C = element.arc.origC;
        seg.r = element.arc.r;
        seg.a0 = element.arc.begAng;
        seg.a1 = element.arc.endAng;
        
        // Вычисляем угол дуги
        double arcAngle = seg.a1 - seg.a0;
        seg.ccw = arcAngle > 0;
        seg.len = std::fabs(arcAngle) * seg.r;
        
        if (seg.len > kEPS) {
            path.segs.Push(seg);
            path.total += seg.len;
        }
        Log("[ShellHelper] Arc parsed: radius=%.3f, angle=%.3f, length=%.3f", 
            seg.r, arcAngle, seg.len);
        return !path.segs.IsEmpty();
    }
        
    else if (element.header.type == API_CircleID) {
        Seg seg;
        seg.type = SegType::Arc;
        seg.C = element.arc.origC;
        seg.r = element.arc.r;
        seg.a0 = 0.0;
        seg.a1 = 2.0 * kPI;
        seg.ccw = true;
        seg.len = 2.0 * kPI * seg.r;
        if (seg.len > kEPS) {
            path.segs.Push(seg);
            path.total += seg.len;
        }
        Log("[ShellHelper] Circle parsed: radius=%.3f, length=%.3f", seg.r, seg.len);
        return !path.segs.IsEmpty();
    }
        
    else if (element.header.type == API_PolyLineID) {
            API_ElementMemo memo;
            BNZeroMemory(&memo, sizeof(memo));
            GSErrCode err = ACAPI_Element_GetMemo(element.header.guid, &memo);
            if (err != NoError || memo.coords == nullptr) {
                ACAPI_DisposeElemMemoHdls(&memo);
                Log("[ShellHelper] ERROR: Не удалось получить memo для полилинии");
                return false;
            }

            const Int32 nCoordsAll = (Int32)(BMGetHandleSize((GSHandle)memo.coords) / (Int32)sizeof(API_Coord));
            const Int32 nCoords = std::max<Int32>(0, nCoordsAll - 1);
            if (nCoords < 2) {
                ACAPI_DisposeElemMemoHdls(&memo);
                Log("[ShellHelper] ERROR: Недостаточно точек в полилинии");
                return false;
            }

            // Карта дуг: индекс начала -> arcAngle
            std::unordered_map<Int32, double> arcByBeg;
            if (memo.parcs != nullptr) {
                const Int32 nArcsAll = (Int32)(BMGetHandleSize((GSHandle)memo.parcs) / (Int32)sizeof(API_PolyArc));
                const Int32 nArcs = std::max<Int32>(0, nArcsAll - 1);
                Log("[ShellHelper] Found %d arcs in polyline", nArcs);
                for (Int32 ai = 1; ai <= nArcs; ++ai) {
                    const API_PolyArc& pa = (*memo.parcs)[ai];
                    Log("[ShellHelper] Arc %d: begIndex=%d, arcAngle=%.6f", ai, pa.begIndex, pa.arcAngle);
                    if (pa.begIndex >= 1 && pa.begIndex <= nCoords - 1) {
                        arcByBeg[pa.begIndex] = pa.arcAngle;
                        Log("[ShellHelper] Added arc to map: begIndex=%d, arcAngle=%.6f", pa.begIndex, pa.arcAngle);
                    } else {
                        Log("[ShellHelper] Skipped arc %d: begIndex=%d out of range [1,%d]", ai, pa.begIndex, nCoords - 1);
                    }
                }
            } else {
                Log("[ShellHelper] No arcs found in polyline (memo.parcs is null)");
            }

            // Перебор сегментов
            API_Coord prev = (*memo.coords)[1];
            for (Int32 idx = 2; idx <= nCoords; ++idx) {
                const API_Coord curr = (*memo.coords)[idx];
                if (NearlyEq(prev, curr)) {
                    prev = curr;
                    continue;
                }

                const Int32 segIdx = idx - 1;
                auto it = arcByBeg.find(segIdx);
                Log("[ShellHelper] Checking segment %d for arcs...", segIdx);

                Seg seg;
                if (it != arcByBeg.end() && std::fabs(it->second) > kEPS) {
                    // дуга
                    seg.type = SegType::Arc;
                    Log("[ShellHelper] Found arc at segment %d: angle=%.6f", segIdx, it->second);
                    if (BuildArcFromPolylineSegment(prev, curr, it->second, seg.C, seg.r, seg.a0, seg.a1, seg.ccw)) {
                        seg.len = std::fabs(seg.a1 - seg.a0) * seg.r;
                        Log("[ShellHelper] Arc built: center=(%.3f,%.3f), radius=%.3f, len=%.3f", 
                            seg.C.x, seg.C.y, seg.r, seg.len);
                    } else {
                        Log("[ShellHelper] Failed to build arc, using line instead");
                        seg.type = SegType::Line;
                        seg.A = prev;
                        seg.B = curr;
                        seg.len = SegLenLine(prev, curr);
                    }
                } else {
                    // линия
                    seg.type = SegType::Line;
                    seg.A = prev;
                    seg.B = curr;
                    seg.len = SegLenLine(prev, curr);
                    if (it != arcByBeg.end()) {
                        Log("[ShellHelper] Line segment %d: len=%.3f (arc angle too small: %.6f)", segIdx, seg.len, it->second);
                    } else {
                        Log("[ShellHelper] Line segment %d: len=%.3f (no arc found)", segIdx, seg.len);
                    }
                }

                if (seg.len > kEPS) {
                    path.segs.Push(seg);
                    path.total += seg.len;
                } else {
                    Log("[ShellHelper] Skipping segment %d: too short (%.6f)", segIdx, seg.len);
                }

                prev = curr;
            }

        ACAPI_DisposeElemMemoHdls(&memo);
        Log("[ShellHelper] Polyline parsed: %d segments, total length=%.3f", 
            (int)path.segs.GetSize(), path.total);
        return !path.segs.IsEmpty();
    }
    else if (element.header.type == API_SplineID) {
        // Базовая поддержка сплайнов - пока создаем простую линию
        API_ElementMemo memo;
        BNZeroMemory(&memo, sizeof(memo));
        GSErrCode err = ACAPI_Element_GetMemo(element.header.guid, &memo);
        if (err != NoError || memo.coords == nullptr) {
            ACAPI_DisposeElemMemoHdls(&memo);
            Log("[ShellHelper] ERROR: Не удалось получить memo для сплайна");
            return false;
        }

        const GSSize coordBytes = BMGetHandleSize((GSHandle)memo.coords);
        const int nFit = (int)(coordBytes / sizeof(API_Coord));
        if (nFit < 2) {
            ACAPI_DisposeElemMemoHdls(&memo);
            Log("[ShellHelper] ERROR: Недостаточно точек в сплайне");
            return false;
        }

        // Создаем сегменты из точек сплайна (пока как простые линии)
        API_Coord prev = (*memo.coords)[0];
        for (int i = 1; i < nFit; ++i) {
            const API_Coord curr = (*memo.coords)[i];
            if (NearlyEq(prev, curr)) {
                prev = curr;
                continue;
            }

            Seg seg;
            seg.type = SegType::Line;
            seg.A = prev;
            seg.B = curr;
            seg.len = SegLenLine(prev, curr);

            if (seg.len > kEPS) {
                path.segs.Push(seg);
                path.total += seg.len;
            }

            prev = curr;
        }

        ACAPI_DisposeElemMemoHdls(&memo);
        Log("[ShellHelper] Spline parsed: %d segments, total length=%.3f", 
            (int)path.segs.GetSize(), path.total);
        return !path.segs.IsEmpty();
    }
    else {
        Log("[ShellHelper] ERROR: Неподдерживаемый тип элемента для парсинга");
        return false;
    }
}

// =============== Создание 3D оболочки через Ruled Shell ===============
bool Create3DShellFromPath(const PathData& path, double widthMM, double stepMM)
{
    Log("[ShellHelper] Create3DShellFromPath: %d сегментов, width=%.1fmm, step=%.1fmm", 
        (int)path.segs.GetSize(), widthMM, stepMM);
    
    if (path.segs.IsEmpty()) {
        Log("[ShellHelper] ERROR: Нет сегментов для обработки");
        return false;
    }
    
    // Создаем 3D точки с заданным шагом
    double step = stepMM / 1000.0; // шаг в метрах
    GS::Array<API_Coord3D> leftPoints;
    GS::Array<API_Coord3D> rightPoints;
    
    // ВСЕГДА добавляем первую точку (позиция 0.0)
    bool firstPointAdded = false;
    bool lastPointAdded = false;
    
    double currentPos = 0.0;
    while (currentPos <= path.total) {
        // Получаем точку на базовой линии
        API_Coord pointOnPath;
        double tangentAngle = 0.0;
        
        // Находим нужный сегмент и позицию в нем
        double accumulatedLength = 0.0;
        bool found = false;
        
        for (UIndex i = 0; i < path.segs.GetSize() && !found; ++i) {
            const Seg& seg = path.segs[i];
            
            if (currentPos <= accumulatedLength + seg.len) {
                // Текущая позиция находится в этом сегменте
                double localPos = currentPos - accumulatedLength;
                
                // Вычисляем точку и угол в зависимости от типа сегмента
                switch (seg.type) {
                    case SegType::Line: {
                        double t = (seg.len > 1e-9) ? localPos / seg.len : 0.0;
                        pointOnPath.x = seg.A.x + t * (seg.B.x - seg.A.x);
                        pointOnPath.y = seg.A.y + t * (seg.B.y - seg.A.y);
                        tangentAngle = std::atan2(seg.B.y - seg.A.y, seg.B.x - seg.A.x);
                        break;
                    }
                    case SegType::Arc: {
                        double t = (seg.len > 1e-9) ? localPos / seg.len : 0.0;
                        double angle = seg.a0 + t * (seg.a1 - seg.a0);
                        pointOnPath.x = seg.C.x + seg.r * std::cos(angle);
                        pointOnPath.y = seg.C.y + seg.r * std::sin(angle);
                        tangentAngle = angle + ((seg.a1 > seg.a0) ? kPI / 2.0 : -kPI / 2.0);
                        break;
                    }
                    default:
                        // Для сплайнов пока используем линейную интерполяцию
                        double t = (seg.len > 1e-9) ? localPos / seg.len : 0.0;
                        pointOnPath.x = seg.A.x + t * (seg.B.x - seg.A.x);
                        pointOnPath.y = seg.A.y + t * (seg.B.y - seg.A.y);
                        tangentAngle = std::atan2(seg.B.y - seg.A.y, seg.B.x - seg.A.x);
                        break;
                }
                found = true;
            }
            accumulatedLength += seg.len;
        }
        
        if (!found) {
            // Если не нашли сегмент, используем последнюю точку
            const Seg& lastSeg = path.segs[path.segs.GetSize() - 1];
            pointOnPath.x = lastSeg.B.x;
            pointOnPath.y = lastSeg.B.y;
            tangentAngle = std::atan2(lastSeg.B.y - lastSeg.A.y, lastSeg.B.x - lastSeg.A.x);
        }
        
        // Вычисляем перпендикулярное направление для каждой конкретной точки
        double perpAngle = tangentAngle + kPI / 2.0;
        double perpX = std::cos(perpAngle);
        double perpY = std::sin(perpAngle);
        
        // Логируем угол для отладки
        if (leftPoints.GetSize() <= 5 || leftPoints.GetSize() % 5 == 0) {
            Log("[ShellHelper] Точка %d: позиция=(%.3f, %.3f), угол касательной=%.3f°, перпендикуляр=%.3f°", 
                (int)leftPoints.GetSize() + 1, pointOnPath.x, pointOnPath.y, 
                tangentAngle * 180.0 / kPI, perpAngle * 180.0 / kPI);
        }
        
        double halfWidth = widthMM / 2000.0; // Переводим мм в метры и делим пополам
        
        // Создаем левую и правую точки
        API_Coord3D leftPoint = {pointOnPath.x + perpX * halfWidth, pointOnPath.y + perpY * halfWidth, 0.0};
        API_Coord3D rightPoint = {pointOnPath.x - perpX * halfWidth, pointOnPath.y - perpY * halfWidth, 0.0};
        
        // Получаем Z-координаты от Mesh
        // ОПТИМИЗАЦИЯ: получаем Z только один раз в начале линии
        static double cachedZ = 0.0;
        static bool zCached = false;
        
        if (!zCached) {
            // Получаем Z-координату только для первой точки
            double z = 0.0;
            API_Vector3D normal = {};
            
            if (GroundHelper::GetGroundZAndNormal(leftPoint, z, normal)) {
                cachedZ = z;
                zCached = true;
                Log("[ShellHelper] Z-координата получена и кэширована: %.3f", cachedZ);
            } else {
                Log("[ShellHelper] WARNING: Не удалось получить Z от Mesh");
                cachedZ = 0.0;
                zCached = true;
            }
        }
        
        // Используем кэшированную Z-координату для обеих точек
        leftPoint.z = cachedZ;
        rightPoint.z = cachedZ;
        
        leftPoints.Push(leftPoint);
        rightPoints.Push(rightPoint);
        
        // Логируем первые и последние точки для отладки
        if (leftPoints.GetSize() <= 5 || leftPoints.GetSize() % 10 == 0) {
            Log("[ShellHelper] Точка %d: left(%.3f, %.3f, %.3f), right(%.3f, %.3f, %.3f)", 
                (int)leftPoints.GetSize(), leftPoint.x, leftPoint.y, leftPoint.z, rightPoint.x, rightPoint.y, rightPoint.z);
        }
        
        currentPos += step;
    }
    
    // ВСЕГДА добавляем последнюю точку (позиция path.total), если она еще не добавлена
    if (!lastPointAdded) {
        // Получаем последнюю точку базовой линии
        API_Coord pointOnPath;
        double tangentAngle = 0.0;
        
        // Находим последний сегмент
        const Seg& lastSeg = path.segs[path.segs.GetSize() - 1];
        if (lastSeg.type == SegType::Line) {
            pointOnPath = lastSeg.B;
            tangentAngle = std::atan2(lastSeg.B.y - lastSeg.A.y, lastSeg.B.x - lastSeg.A.x);
        } else {
            // Arc
            pointOnPath.x = lastSeg.C.x + lastSeg.r * std::cos(lastSeg.a1);
            pointOnPath.y = lastSeg.C.y + lastSeg.r * std::sin(lastSeg.a1);
            tangentAngle = lastSeg.a1 + ((lastSeg.a1 - lastSeg.a0 >= 0.0) ? kPI / 2.0 : -kPI / 2.0);
        }
        
        // Вычисляем перпендикулярное направление для последней точки
        double perpAngle = tangentAngle + kPI / 2.0;
        double perpX = std::cos(perpAngle);
        double perpY = std::sin(perpAngle);
        
        double halfWidth = widthMM / 2000.0;
        
        // Создаем левую и правую точки для последней точки
        API_Coord3D leftPoint = {pointOnPath.x + perpX * halfWidth, pointOnPath.y + perpY * halfWidth, 0.0};
        API_Coord3D rightPoint = {pointOnPath.x - perpX * halfWidth, pointOnPath.y - perpY * halfWidth, 0.0};
        
        // Используем кэшированную Z-координату (получаем из статической переменной)
        static double cachedZ = 0.0;
        static bool zCached = false;
        
        if (!zCached) {
            // Получаем Z-координату только для первой точки
            double z = 0.0;
            API_Vector3D normal = {};
            
            if (GroundHelper::GetGroundZAndNormal(leftPoint, z, normal)) {
                cachedZ = z;
                zCached = true;
                Log("[ShellHelper] Z-координата получена и кэширована: %.3f", cachedZ);
            } else {
                Log("[ShellHelper] WARNING: Не удалось получить Z от Mesh");
                cachedZ = 0.0;
                zCached = true;
            }
        }
        
        leftPoint.z = cachedZ;
        rightPoint.z = cachedZ;
        
        leftPoints.Push(leftPoint);
        rightPoints.Push(rightPoint);
        
        Log("[ShellHelper] Последняя точка добавлена: left(%.3f, %.3f, %.3f), right(%.3f, %.3f, %.3f)", 
            leftPoint.x, leftPoint.y, leftPoint.z, rightPoint.x, rightPoint.y, rightPoint.z);
    }
    
    Log("[ShellHelper] Создано %d пар точек для 3D оболочки", (int)leftPoints.GetSize());
    
    // Создаем одну плавную Shell поверхность, которая огибает Mesh
    // Объединяем левые и правые точки в один контур для создания плавной поверхности
    GS::Array<API_Coord3D> allPoints;
    
    // Добавляем точки по периметру: левая сторона -> правая сторона (в обратном порядке)
    for (UIndex i = 0; i < leftPoints.GetSize(); ++i) {
        allPoints.Push(leftPoints[i]);
    }
    for (Int32 i = (Int32)rightPoints.GetSize() - 1; i >= 0; --i) {
        allPoints.Push(rightPoints[i]);
    }
    
            Log("[ShellHelper] Создаем замкнутый контур из %d точек", (int)allPoints.GetSize());
            
            // Создаем замкнутый контур: левые точки + правые точки в обратном порядке
            GS::Array<API_Coord> closedContourPoints;
            
            // Добавляем левые точки от начала до конца
            for (UIndex i = 0; i < leftPoints.GetSize(); ++i) {
                closedContourPoints.Push({leftPoints[i].x, leftPoints[i].y});
            }
            
            // Добавляем правые точки от конца до начала (в обратном порядке)
            for (Int32 i = rightPoints.GetSize() - 1; i >= 0; --i) {
                closedContourPoints.Push({rightPoints[i].x, rightPoints[i].y});
            }
            
            Log("[ShellHelper] Замкнутый контур: %d точек", (int)closedContourPoints.GetSize());
            if (closedContourPoints.GetSize() > 0) {
                Log("[ShellHelper] Первая точка (%.3f, %.3f), последняя (%.3f, %.3f)", 
                    closedContourPoints[0].x, closedContourPoints[0].y,
                    closedContourPoints[closedContourPoints.GetSize()-1].x, closedContourPoints[closedContourPoints.GetSize()-1].y);
            }
            
            // Создаем один замкнутый Spline
            API_Guid closedSplineGuid = CreateSplineFromPoints(closedContourPoints);
            if (closedSplineGuid == APINULLGuid) {
                Log("[ShellHelper] ERROR: Не удалось создать замкнутый Spline");
                return false;
            }
            
            Log("[ShellHelper] SUCCESS: Создан замкнутый Spline контур");
            return true;
}

// =============== Создание Spline из 2D точек ===============
API_Guid CreateSplineFromPoints(const GS::Array<API_Coord>& points)
{
    if (points.GetSize() < 2) {
        Log("[ShellHelper] ERROR: Недостаточно точек для создания Spline (нужно минимум 2)");
        return APINULLGuid;
    }
    
    Log("[ShellHelper] CreateSplineFromPoints: создаем Spline с %d точками", (int)points.GetSize());
    
    API_Element spline = {};
    spline.header.type = API_SplineID;
    GSErrCode err = ACAPI_Element_GetDefaults(&spline, nullptr);
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось получить настройки по умолчанию для Spline, err=%d", (int)err);
        return APINULLGuid;
    }
    
    // Создаем memo для Spline
    API_ElementMemo memo = {};
    BNZeroMemory(&memo, sizeof(API_ElementMemo));
    
    const Int32 nUnique = (Int32)points.GetSize();
    const Int32 nCoords = nUnique; // Без замыкающей точки для Spline
    
    // Выделяем память для координат (1-based indexing!)
    // Выделяем nCoords + 1 элементов, но используем только индексы от 1 до nCoords
    memo.coords = reinterpret_cast<API_Coord**>(BMAllocateHandle((nCoords + 1) * (GSSize)sizeof(API_Coord), ALLOCATE_CLEAR, 0));
    if (memo.coords == nullptr) {
        Log("[ShellHelper] ERROR: Не удалось выделить память для координат Spline");
        return APINULLGuid;
    }
    
    // Выделяем память для направлений Безье
    memo.bezierDirs = reinterpret_cast<API_SplineDir**>(BMAllocateHandle((nCoords + 1) * (GSSize)sizeof(API_SplineDir), ALLOCATE_CLEAR, 0));
    if (memo.bezierDirs == nullptr) {
        ACAPI_DisposeElemMemoHdls(&memo);
        Log("[ShellHelper] ERROR: Не удалось выделить память для bezierDirs");
        return APINULLGuid;
    }
    
    // Заполняем координаты (1-based indexing!)
    for (Int32 i = 0; i < nUnique; ++i) {
        (*memo.coords)[i + 1] = points[i];
        
        if (i < 5 || i >= nUnique - 5) { // Логируем первые и последние 5 точек
            Log("[ShellHelper] Spline Point %d: (%.3f, %.3f)", i+1, points[i].x, points[i].y);
        }
    }
    
    
    // Не замыкаем контур для Spline (это нужно только для полигонов)
    
    // Настраиваем направления Безье для плавного Spline
    for (Int32 i = 1; i <= nCoords; ++i) {
        API_SplineDir& dir = (*memo.bezierDirs)[i];
        
        if (i == 1) {
            // Первая точка - направление к следующей
            API_Coord next = (*memo.coords)[2];
            API_Coord curr = (*memo.coords)[1];
            double dx = next.x - curr.x;
            double dy = next.y - curr.y;
            double len = sqrt(dx*dx + dy*dy);
            if (len > 1e-9) {
                dir.dirAng = atan2(dy, dx);
                dir.lenNext = len * 0.3; // 30% от длины сегмента
                dir.lenPrev = 0.0;
            } else {
                dir.dirAng = 0.0;
                dir.lenNext = 0.0;
                dir.lenPrev = 0.0;
            }
        } else if (i == nCoords) {
            // Последняя точка - направление от предыдущей
            API_Coord prev = (*memo.coords)[nCoords-1];
            API_Coord curr = (*memo.coords)[nCoords];
            double dx = curr.x - prev.x;
            double dy = curr.y - prev.y;
            double len = sqrt(dx*dx + dy*dy);
            if (len > 1e-9) {
                dir.dirAng = atan2(dy, dx);
                dir.lenNext = 0.0;
                dir.lenPrev = len * 0.3; // 30% от длины сегмента
            } else {
                dir.dirAng = 0.0;
                dir.lenNext = 0.0;
                dir.lenPrev = 0.0;
            }
        } else {
            // Средние точки - направление между предыдущей и следующей
            API_Coord prev = (*memo.coords)[i-1];
            API_Coord curr = (*memo.coords)[i];
            API_Coord next = (*memo.coords)[i+1];
            
            double dx1 = curr.x - prev.x;
            double dy1 = curr.y - prev.y;
            double dx2 = next.x - curr.x;
            double dy2 = next.y - curr.y;
            
            double len1 = sqrt(dx1*dx1 + dy1*dy1);
            double len2 = sqrt(dx2*dx2 + dy2*dy2);
            
            if (len1 > 1e-9 && len2 > 1e-9) {
                double ang1 = atan2(dy1, dx1);
                double ang2 = atan2(dy2, dx2);
                dir.dirAng = (ang1 + ang2) * 0.5; // Среднее направление
                dir.lenNext = len2 * 0.3;
                dir.lenPrev = len1 * 0.3;
            } else {
                dir.dirAng = 0.0;
                dir.lenNext = 0.0;
                dir.lenPrev = 0.0;
            }
        }
    }
    
    
    // Создаем элемент внутри Undo-команды
    err = ACAPI_CallUndoableCommand("Create Spline", [&]() -> GSErrCode {
        return ACAPI_Element_Create(&spline, &memo);
    });
    ACAPI_DisposeElemMemoHdls(&memo);
    
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось создать Spline, err=%d", (int)err);
        return APINULLGuid;
    }
    
    Log("[ShellHelper] SUCCESS: Создан Spline с %d точками", (int)points.GetSize());
    return spline.header.guid;
}

// =============== Создание Shell с 3D точками ===============
API_Guid Create3DShell(const GS::Array<API_Coord3D>& points)
{
    if (points.GetSize() < 3) {
        Log("[ShellHelper] ERROR: Недостаточно точек для создания Shell (нужно минимум 3)");
        return APINULLGuid;
    }
    
    Log("[ShellHelper] Create3DShell: создаем плавную 3D Shell поверхность с %d точками", (int)points.GetSize());
    
    API_Element shell = {};
    shell.header.type = API_ShellID;
    GSErrCode err = ACAPI_Element_GetDefaults(&shell, nullptr);
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось получить настройки по умолчанию для Shell, err=%d", (int)err);
        return APINULLGuid;
    }
    
    // Создаем memo для Shell
    API_ElementMemo memo = {};
    BNZeroMemory(&memo, sizeof(API_ElementMemo));
    
    const Int32 nUnique = (Int32)points.GetSize();
    const Int32 nCoords = nUnique + 1; // + замыкающая точка
    
    // Выделяем память для координат (1-based indexing!)
    memo.coords = reinterpret_cast<API_Coord**>(BMAllocateHandle((nCoords + 1) * (GSSize)sizeof(API_Coord), ALLOCATE_CLEAR, 0));
    if (memo.coords == nullptr) {
        Log("[ShellHelper] ERROR: Не удалось выделить память для координат Shell");
        return APINULLGuid;
    }
    
    // Заполняем 2D контур (1-based indexing!) с логированием для отладки
    for (Int32 i = 0; i < nUnique; ++i) {
        (*memo.coords)[i + 1] = {points[i].x, points[i].y};
        
        if (i < 5 || i >= nUnique - 5) { // Логируем первые и последние 5 точек
            Log("[ShellHelper] Point %d: (%.3f, %.3f, %.3f)", i+1, points[i].x, points[i].y, points[i].z);
        }
    }
    // Замыкаем контур
    (*memo.coords)[nCoords] = (*memo.coords)[1];
    
    // Для Shell Z-координаты задаются через высоты точек
    // Пока создаем простой Shell только с 2D контуром
    
    // Настраиваем контуры
    memo.pends = reinterpret_cast<Int32**>(BMAllocateHandle(2 * (GSSize)sizeof(Int32), ALLOCATE_CLEAR, 0));
    if (memo.pends == nullptr) {
        ACAPI_DisposeElemMemoHdls(&memo);
        Log("[ShellHelper] ERROR: Не удалось выделить память для pends");
        return APINULLGuid;
    }
    (*memo.pends)[0] = 0;
    (*memo.pends)[1] = nCoords;
    
    // Настраиваем Shell для создания плавной поверхности
    // Для Shell настройки делаются через memo, не нужно менять shell.shell.poly
    
    // Создаем элемент внутри Undo-команды
    err = ACAPI_CallUndoableCommand("Create 3D Shell", [&]() -> GSErrCode {
        return ACAPI_Element_Create(&shell, &memo);
    });
    ACAPI_DisposeElemMemoHdls(&memo);
    
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось создать Shell, err=%d", (int)err);
        return APINULLGuid;
    }
    
    Log("[ShellHelper] SUCCESS: Создана плавная Shell поверхность с %d точками (3D)", (int)points.GetSize());
    return shell.header.guid;
}

// =============== Создание Ruled Shell ===============
bool CreateRuledShell(const API_Guid& leftSplineGuid, const API_Guid& rightSplineGuid)
{
    Log("[ShellHelper] CreateRuledShell: создаем Ruled Shell между двумя Spline");
    
    if (leftSplineGuid == APINULLGuid || rightSplineGuid == APINULLGuid) {
        Log("[ShellHelper] ERROR: Неверные GUID для Spline");
        return false;
    }
    
    // ВРЕМЕННО: создаем простой Shell с прямоугольным контуром
    // TODO: Реализовать правильное создание Ruled Shell с двумя Spline
    
    Log("[ShellHelper] ВРЕМЕННО: создаем простой Shell вместо Ruled Shell");
    
    // Создаем простой прямоугольный контур для Shell
    GS::Array<API_Coord> contour;
    contour.Push({-5.0, -2.0});  // левый нижний
    contour.Push({5.0, -2.0});   // правый нижний
    contour.Push({5.0, 2.0});    // правый верхний
    contour.Push({-5.0, 2.0});   // левый верхний
    
    // Создаем Shell элемент
    API_Element shell = {};
    shell.header.type = API_ShellID;
    
    GSErrCode err = ACAPI_Element_GetDefaults(&shell, nullptr);
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось получить настройки по умолчанию для Shell, err=%d", (int)err);
        return false;
    }
    
    // Создаем memo для Shell
    API_ElementMemo memo = {};
    BNZeroMemory(&memo, sizeof(API_ElementMemo));
    
    const Int32 nCoords = (Int32)contour.GetSize() + 1; // + замыкающая точка
    
    // Выделяем память для координат (1-based indexing!)
    memo.coords = reinterpret_cast<API_Coord**>(BMAllocateHandle((nCoords + 1) * (GSSize)sizeof(API_Coord), ALLOCATE_CLEAR, 0));
    if (memo.coords == nullptr) {
        Log("[ShellHelper] ERROR: Не удалось выделить память для координат Shell");
        return false;
    }
    
    // Заполняем координаты (1-based indexing!)
    for (Int32 i = 0; i < (Int32)contour.GetSize(); ++i) {
        (*memo.coords)[i + 1] = contour[i];
    }
    (*memo.coords)[nCoords] = (*memo.coords)[1]; // замкнуть контур
    
    // Настраиваем контуры
    memo.pends = reinterpret_cast<Int32**>(BMAllocateHandle(2 * (GSSize)sizeof(Int32), ALLOCATE_CLEAR, 0));
    if (memo.pends == nullptr) {
        ACAPI_DisposeElemMemoHdls(&memo);
        Log("[ShellHelper] ERROR: Не удалось выделить память для pends");
        return false;
    }
    (*memo.pends)[0] = 0;
    (*memo.pends)[1] = nCoords;
    
    // Настраиваем Shell
    // Для Shell настройки делаются через memo, не нужно менять shellClass
    
    // Создаем элемент
    err = ACAPI_Element_Create(&shell, &memo);
    ACAPI_DisposeElemMemoHdls(&memo);
    
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось создать Shell, err=%d", (int)err);
        return false;
    }
    
    Log("[ShellHelper] SUCCESS: Shell создан успешно (временная реализация)");
    return true;
}

// =============== Создание перпендикулярных линий от сегментов ===============
bool CreatePerpendicularLinesFromSegments(const PathData& path, double widthMM)
{
    Log("[ShellHelper] CreatePerpendicularLinesFromSegments: %d сегментов, width=%.1fmm", 
        (int)path.segs.GetSize(), widthMM);
    
    if (path.segs.IsEmpty()) {
        Log("[ShellHelper] ERROR: Нет сегментов для обработки");
        return false;
    }
    
    double halfWidth = widthMM / 2000.0; // Переводим мм в метры и делим пополам
    
    ACAPI_CallUndoableCommand("Create Shell Lines", [&] () -> GSErrCode {
        
        // Создаем перпендикулярные линии с шагом
        // ОПТИМИЗАЦИЯ: увеличиваем шаг для уменьшения количества вызовов GetGroundZAndNormal
        double step = 2.0; // шаг в метрах (увеличен до 2.0 для производительности)
        double currentPos = 0.0;
        
        // ОПТИМИЗАЦИЯ: получаем Z-координату только один раз в начале линии
        API_Coord3D firstPoint = {0.0, 0.0, 0.0};
        double cachedZ = 0.0;
        bool zCached = false;
        
        while (currentPos <= path.total) {
            // Используем функцию EvalOnPath для получения точки и угла
            API_Coord pointOnPath;
            double tangentAngle = 0.0;
            
            // Находим нужный сегмент и позицию в нем
            double accumulatedLength = 0.0;
            bool found = false;
            
            for (UIndex i = 0; i < path.segs.GetSize() && !found; ++i) {
                const Seg& seg = path.segs[i];
                
                if (currentPos <= accumulatedLength + seg.len) {
                    // Текущая позиция находится в этом сегменте
                    double localPos = currentPos - accumulatedLength;
                    
                    // Вычисляем точку и угол в зависимости от типа сегмента
                    switch (seg.type) {
                        case SegType::Line: {
                            double t = (seg.len > 1e-9) ? localPos / seg.len : 0.0;
                            pointOnPath.x = seg.A.x + t * (seg.B.x - seg.A.x);
                            pointOnPath.y = seg.A.y + t * (seg.B.y - seg.A.y);
                            tangentAngle = std::atan2(seg.B.y - seg.A.y, seg.B.x - seg.A.x);
                            break;
                        }
                        case SegType::Arc: {
                            double t = (seg.len > 1e-9) ? localPos / seg.len : 0.0;
                            double angle = seg.a0 + t * (seg.a1 - seg.a0);
                            pointOnPath.x = seg.C.x + seg.r * std::cos(angle);
                            pointOnPath.y = seg.C.y + seg.r * std::sin(angle);
                            tangentAngle = angle + ((seg.a1 > seg.a0) ? kPI / 2.0 : -kPI / 2.0);
                            break;
                        }
                        default:
                            // Для сплайнов пока используем линейную интерполяцию
                            Log("[ShellHelper] WARNING: Сплайны пока не поддерживаются, используем линейную интерполяцию");
                            double t = (seg.len > 1e-9) ? localPos / seg.len : 0.0;
                            pointOnPath.x = seg.A.x + t * (seg.B.x - seg.A.x);
                            pointOnPath.y = seg.A.y + t * (seg.B.y - seg.A.y);
                            tangentAngle = std::atan2(seg.B.y - seg.A.y, seg.B.x - seg.A.x);
                            break;
                    }
                    found = true;
                }
                accumulatedLength += seg.len;
            }
            
            if (!found) {
                // Если не нашли сегмент, используем последнюю точку
                const Seg& lastSeg = path.segs[path.segs.GetSize() - 1];
                pointOnPath.x = lastSeg.B.x;
                pointOnPath.y = lastSeg.B.y;
                tangentAngle = std::atan2(lastSeg.B.y - lastSeg.A.y, lastSeg.B.x - lastSeg.A.x);
            }
            
            // Получаем Z-координату от Mesh
            // ОПТИМИЗАЦИЯ: получаем Z только один раз в начале линии
            API_Coord3D point3D = {pointOnPath.x, pointOnPath.y, 0.0};
            
            if (!zCached) {
                // Получаем Z-координату только для первой точки
                double z = 0.0;
                API_Vector3D normal = {};
                
                if (GroundHelper::GetGroundZAndNormal(point3D, z, normal)) {
                    cachedZ = z;
                    zCached = true;
                    Log("[ShellHelper] Point (%.3f, %.3f, %.3f) - Z from Mesh (cached for all points)", point3D.x, point3D.y, cachedZ);
                } else {
                    Log("[ShellHelper] WARNING: Не удалось получить Z от Mesh для точки (%.3f, %.3f)", point3D.x, point3D.y);
                    cachedZ = 0.0;
                    zCached = true;
                }
            }
            
            point3D.z = cachedZ;
            Log("[ShellHelper] Point (%.3f, %.3f, %.3f) - Z from cache", point3D.x, point3D.y, point3D.z);
            
            // Вычисляем перпендикулярное направление (поворот на 90 градусов)
            double perpAngle = tangentAngle + kPI / 2.0;
            double perpX = std::cos(perpAngle);
            double perpY = std::sin(perpAngle);
            
            // Создаем перпендикулярную линию
            API_Element line = {};
            line.header.type = API_LineID;
            GSErrCode err = ACAPI_Element_GetDefaults(&line, nullptr);
            if (err != NoError) continue;
            
            line.header.floorInd = 0; // TODO: получить правильный этаж
            
            // Создаем перпендикулярную линию с полученной Z-координатой
            line.line.begC.x = pointOnPath.x + perpX * halfWidth;
            line.line.begC.y = pointOnPath.y + perpY * halfWidth;
            line.line.endC.x = pointOnPath.x - perpX * halfWidth;
            line.line.endC.y = pointOnPath.y - perpY * halfWidth;
            
            err = ACAPI_Element_Create(&line, nullptr);
            if (err != NoError) {
                Log("[ShellHelper] ERROR: Не удалось создать перпендикулярную линию, err=%d", (int)err);
            } else {
                Log("[ShellHelper] Создана перпендикулярная линия в точке (%.3f, %.3f, %.3f)", point3D.x, point3D.y, point3D.z);
            }
            
            currentPos += step;
        }
        
        Log("[ShellHelper] SUCCESS: Перпендикулярные линии созданы для всех сегментов");
        return NoError;
    });
    
    return true;
}

// =============== Выбор Mesh поверхности ===============
bool SetMeshSurfaceForShell()
{
    Log("[ShellHelper] SetMeshSurfaceForShell: выбор Mesh поверхности");
    
    // Используем существующую функцию GroundHelper::SetGroundSurface()
    bool success = GroundHelper::SetGroundSurface();
    
    if (success) {
        // Получаем GUID выбранной Mesh поверхности из выделения
        API_SelectionInfo selectionInfo;
        GS::Array<API_Neig> selNeigs;
        GSErrCode err = ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
        
        if (err == NoError && selNeigs.GetSize() > 0) {
            g_meshSurfaceGuid = selNeigs[0].guid;
            Log("[ShellHelper] Mesh поверхность выбрана успешно, GUID: %s", 
                APIGuidToString(g_meshSurfaceGuid).ToCStr().Get());
        } else {
            Log("[ShellHelper] Ошибка получения GUID выбранной Mesh поверхности");
            g_meshSurfaceGuid = APINULLGuid;
        }
    } else {
        Log("[ShellHelper] Ошибка выбора Mesh поверхности");
        g_meshSurfaceGuid = APINULLGuid;
    }
    
    return success;
}

// =============== Получение 3D точек вдоль базовой линии ===============
GS::Array<API_Coord3D> Get3DPointsAlongBaseLine(double stepMM)
{
    Log("[ShellHelper] Get3DPointsAlongBaseLine: step=%.1fmm", stepMM);
    
    GS::Array<API_Coord3D> points;
    
    if (g_baseLineGuid == APINULLGuid) {
        Log("[ShellHelper] ERROR: Базовая линия не выбрана");
        return points;
    }
    
    // Получаем элемент базовой линии
    API_Element element = {};
    element.header.guid = g_baseLineGuid;
    GSErrCode err = ACAPI_Element_Get(&element);
    if (err != NoError) {
        Log("[ShellHelper] ERROR: Не удалось получить элемент базовой линии");
        return points;
    }
    
    // Парсим элемент в сегменты
    PathData path;
    if (!ParseElementToSegments(element, path)) {
        Log("[ShellHelper] ERROR: Не удалось распарсить элемент в сегменты");
        return points;
    }
    
    Log("[ShellHelper] Элемент распарсен: %d сегментов, общая длина %.3fм", 
        (int)path.segs.GetSize(), path.total);
    
    // Генерируем точки вдоль пути с заданным шагом
    double step = stepMM / 1000.0; // конвертируем в метры
    double currentPos = 0.0;
    
    while (currentPos <= path.total) {
        // Находим точку на пути для текущей позиции
        API_Coord3D point = {0.0, 0.0, 0.0};
        
        // TODO: Реализовать получение точки на пути по позиции
        // Это потребует реализации функции EvalOnPath аналогично MarkupHelper
        
        points.Push(point);
        currentPos += step;
    }
    
    Log("[ShellHelper] Сгенерировано %d точек вдоль базовой линии", (int)points.GetSize());
    return points;
}

// =============== Создание перпендикулярных 3D точек ===============
bool CreatePerpendicular3DPoints(double widthMM, double stepMM, 
                                GS::Array<API_Coord3D>& leftPoints, 
                                GS::Array<API_Coord3D>& rightPoints)
{
    Log("[ShellHelper] CreatePerpendicular3DPoints: width=%.1fmm, step=%.1fmm", widthMM, stepMM);
    
    if (g_baseLineGuid == APINULLGuid) {
        Log("[ShellHelper] ERROR: Базовая линия не выбрана");
        return false;
    }
    
    if (g_meshSurfaceGuid == APINULLGuid) {
        Log("[ShellHelper] ERROR: Mesh поверхность не выбрана");
        return false;
    }
    
    // Получаем точки вдоль базовой линии
    GS::Array<API_Coord3D> basePoints = Get3DPointsAlongBaseLine(stepMM);
    if (basePoints.IsEmpty()) {
        Log("[ShellHelper] ERROR: Не удалось получить точки вдоль базовой линии");
        return false;
    }
    
    double halfWidth = widthMM / 2000.0; // конвертируем в метры и делим пополам
    
    leftPoints.Clear();
    rightPoints.Clear();
    
    // Для каждой точки базовой линии создаем перпендикулярные точки
    for (UIndex i = 0; i < basePoints.GetSize(); ++i) {
        const API_Coord3D& basePoint = basePoints[i];
        
        // TODO: Вычислить направление касательной к пути в этой точке
        // Пока используем простое направление по X
        API_Coord3D tangent = {1.0, 0.0, 0.0};
        API_Coord3D perpendicular = {-tangent.y, tangent.x, 0.0};
        
        // Нормализуем перпендикулярный вектор
        double len = std::sqrt(perpendicular.x * perpendicular.x + perpendicular.y * perpendicular.y);
        if (len > kEPS) {
            perpendicular.x /= len;
            perpendicular.y /= len;
        }
        
        // Создаем левую и правую точки
        API_Coord3D leftPoint = {
            basePoint.x + perpendicular.x * halfWidth,
            basePoint.y + perpendicular.y * halfWidth,
            basePoint.z
        };
        
        API_Coord3D rightPoint = {
            basePoint.x - perpendicular.x * halfWidth,
            basePoint.y - perpendicular.y * halfWidth,
            basePoint.z
        };
        
        // Получаем Z-координаты от Mesh
        double leftZ = 0.0, rightZ = 0.0;
        API_Vector3D leftNormal = {}, rightNormal = {};
        
        if (GroundHelper::GetGroundZAndNormal(leftPoint, leftZ, leftNormal)) {
            leftPoint.z = leftZ;
        }
        
        if (GroundHelper::GetGroundZAndNormal(rightPoint, rightZ, rightNormal)) {
            rightPoint.z = rightZ;
        }
        
        leftPoints.Push(leftPoint);
        rightPoints.Push(rightPoint);
    }
    
    Log("[ShellHelper] Создано %d левых и %d правых перпендикулярных точек", 
        (int)leftPoints.GetSize(), (int)rightPoints.GetSize());
    
    return true;
}

// =============== Создание 3D Spline ===============
bool Create3DSpline(const GS::Array<API_Coord3D>& points, const GS::UniString& name)
{
    Log("[ShellHelper] Create3DSpline: создание 3D Spline из %d точек", (int)points.GetSize());
    
    if (points.IsEmpty()) {
        Log("[ShellHelper] ERROR: Нет точек для создания Spline");
        return false;
    }
    
    // TODO: Реализовать создание 3D Spline из массива точек
    // Это потребует изучения API для создания Spline элементов
    
    Log("[ShellHelper] TODO: Создание 3D Spline не реализовано");
    return false;
}

} // namespace ShellHelper
