#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ShellHelper.hpp"
#include "APICommon.h"
#include "LandscapeHelper.hpp"
#include "GroundHelper.hpp"
#include "BrowserRepl.hpp"
#include <cstdarg>

namespace ShellHelper {

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
        Log("[ShellHelper] Базовая линия выбрана успешно");
    } else {
        Log("[ShellHelper] Ошибка выбора базовой линии");
    }
    
    return success;
}

// =============== Основная функция создания оболочки ===============
bool CreateShellFromLine(double widthMM, double stepMM)
{
    // Простейший тест - создаем одну линию
    Log("[ShellHelper] CreateShellFromLine: START");
    
    // Создаем простую линию для теста
    API_Element line = {};
    line.header.type = API_LineID;
    
    // Устанавливаем обязательные поля вручную (как в примерах)
    line.header.floorInd = 0;  // Этаж 0
    line.header.layer = APIApplicationLayerAttributeIndex;  // Слой приложения
    line.line.linePen.penIndex = 1;  // Перо 1
    line.line.ltypeInd = APISolidLineAttributeIndex;  // Сплошная линия
    
    // Координаты линии
    line.line.begC = {0.0, 0.0};
    line.line.endC = {1.0, 0.0};
    
    Log("[ShellHelper] Создание тестовой линии: begC=(%.3f,%.3f), endC=(%.3f,%.3f), layer=%s, pen=%d", 
        line.line.begC.x, line.line.begC.y, line.line.endC.x, line.line.endC.y, 
        line.header.layer.ToUniString().ToCStr().Get(), line.line.linePen.penIndex);
    
    GSErrCode createErr = ACAPI_Element_Create(&line, nullptr);
    Log("[ShellHelper] ACAPI_Element_Create: err=%d", (int)createErr);
    
    if (createErr == NoError) {
        Log("[ShellHelper] SUCCESS: Линия создана");
        return true;
    } else {
        Log("[ShellHelper] FAILED: Ошибка создания линии err=%d", (int)createErr);
        return false;
    }
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

} // namespace ShellHelper
