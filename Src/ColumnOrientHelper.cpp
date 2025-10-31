// ============================================================================
// ColumnOrientHelper.cpp — ориентация колонн по поверхности mesh
// ============================================================================

#include "ColumnOrientHelper.hpp"
#include "MeshIntersectionHelper.hpp"
#include "GroundHelper.hpp"
#include "BrowserRepl.hpp"

#include "ACAPinc.h"
#include "APICommon.h"

#include <cmath>
#include <cstdarg>

// ------------------ Globals ------------------
static GS::Array<API_Guid> g_columnGuids;
static GS::Array<API_Guid> g_beamGuids;
static API_Guid g_meshGuid = APINULLGuid;

// ------------------ Logging ------------------
static inline void Log(const char* fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    char buf[4096];
    std::vsnprintf(buf, sizeof(buf), fmt, vl);
    va_end(vl);

    GS::UniString s(buf);
    if (BrowserRepl::HasInstance())
        BrowserRepl::GetInstance().LogToBrowser(s);
    ACAPI_WriteReport("%s", false, s.ToCStr().Get());
}

// ================================================================
// Public API
// ================================================================

bool ColumnOrientHelper::SetColumns()
{
    Log("[ColumnOrient] SetColumns ENTER");
    g_columnGuids.Clear();

    API_SelectionInfo selInfo{};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    Log("[ColumnOrient] neigs=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{};
        el.header.guid = n.guid;
        if (ACAPI_Element_Get(&el) != NoError) continue;
        
        if (el.header.type.typeID == API_ColumnID) {
            g_columnGuids.Push(n.guid);
            Log("[ColumnOrient] accept column %s", APIGuidToString(n.guid).ToCStr().Get());
        }
    }

    Log("[ColumnOrient] SetColumns EXIT: count=%u", (unsigned)g_columnGuids.GetSize());
    return !g_columnGuids.IsEmpty();
}

bool ColumnOrientHelper::SetBeams()
{
    Log("[BeamOrient] SetBeams ENTER");
    g_beamGuids.Clear();

    API_SelectionInfo selInfo{};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    Log("[BeamOrient] neigs=%d", (int)selNeigs.GetSize());
    for (const API_Neig& n : selNeigs) {
        API_Element el{};
        el.header.guid = n.guid;
        if (ACAPI_Element_Get(&el) != NoError) continue;
        
        if (el.header.type.typeID == API_BeamID) {
            g_beamGuids.Push(n.guid);
            Log("[BeamOrient] accept beam %s", APIGuidToString(n.guid).ToCStr().Get());
        }
    }

    Log("[BeamOrient] SetBeams EXIT: count=%u", (unsigned)g_beamGuids.GetSize());
    return !g_beamGuids.IsEmpty();
}

bool ColumnOrientHelper::SetMesh()
{
    Log("[ColumnOrient] SetMesh ENTER");
    g_meshGuid = APINULLGuid;
    
    // Получаем mesh из выделения
    API_SelectionInfo selInfo{};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);

    for (const API_Neig& n : selNeigs) {
        API_Element el{};
        el.header.guid = n.guid;
        if (ACAPI_Element_Get(&el) != NoError) continue;
        if (el.header.type.typeID == API_MeshID) {
            g_meshGuid = n.guid;
            // Также устанавливаем в GroundHelper для MeshIntersectionHelper
            // Используем прямую установку по GUID
            GroundHelper::SetGroundSurfaceByGuid(n.guid);
            Log("[ColumnOrient] SetMesh: %s", APIGuidToString(n.guid).ToCStr().Get());
            return true;
        }
    }

    Log("[ColumnOrient] SetMesh EXIT: failed - no mesh in selection");
    return false;
}

// Вычисляет угол наклона оси из нормали поверхности
static void ComputeTiltFromNormal(const API_Vector3D& normal, double& outTiltAngle, double& outTiltDirection)
{
    // Нормализуем нормаль на всякий случай
    double len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len < 1e-9) {
        // Вертикальная нормаль (поверхность горизонтальная)
        outTiltAngle = 0.0;
        outTiltDirection = 0.0;
        return;
    }
    
    double nx = normal.x / len;
    double ny = normal.y / len;
    double nz = normal.z / len;
    
    // Угол наклона от вертикали (0 = вертикально вверх, PI/2 = горизонтально)
    // Используем z-компоненту нормали: для вертикальной нормали z=1, для горизонтальной z=0
    outTiltAngle = std::acos(std::max(-1.0, std::min(1.0, nz)));
    
    // Направление наклона в плоскости XY (азимут) - проекция нормали на плоскость XY
    outTiltDirection = std::atan2(ny, nx);
}

bool ColumnOrientHelper::OrientColumnsToSurface()
{
    Log("[ColumnOrient] OrientColumnsToSurface ENTER");
    
    // Если колонны не установлены, но балки есть - используем балки
    if (g_columnGuids.IsEmpty() && !g_beamGuids.IsEmpty()) {
        Log("[ColumnOrient] No columns, but beams found - using OrientBeamsToSurface instead");
        return OrientBeamsToSurface();
    }
    
    if (g_columnGuids.IsEmpty()) {
        Log("[ColumnOrient] ERR: no columns set");
        return false;
    }
    
    if (g_meshGuid == APINULLGuid) {
        Log("[ColumnOrient] ERR: mesh not set, call SetMesh() first");
        return false;
    }

    // Убеждаемся, что mesh установлен в GroundHelper для MeshIntersectionHelper
    // (MeshIntersectionHelper использует GroundHelper внутри)
    
    // Устанавливаем mesh в GroundHelper напрямую по сохранённому GUID
    // Не используем SetGroundSurface(), так как в выделении могут быть только колонны
    if (!GroundHelper::SetGroundSurfaceByGuid(g_meshGuid)) {
        Log("[ColumnOrient] ERR: failed to set mesh in GroundHelper");
        return false;
    }

    const GSErr cmdErr = ACAPI_CallUndoableCommand("Orient Columns to Surface", [&]() -> GSErr {
        UInt32 oriented = 0;
        
        for (const API_Guid& colGuid : g_columnGuids) {
            API_Element col{};
            col.header.guid = colGuid;
            if (ACAPI_Element_Get(&col) != NoError) {
                Log("[ColumnOrient] failed to get column %s", APIGuidToString(colGuid).ToCStr().Get());
                continue;
            }
            
            if (col.header.type.typeID != API_ColumnID) continue;
            
            // Загружаем memo колонны для проверки наличия полей для наклона
            API_ElementMemo memo{};
            bool hasMemo = (ACAPI_Element_GetMemo(colGuid, &memo, APIMemoMask_All) == NoError);

            // Получаем XY координаты колонны
            API_Coord xy = col.column.origoPos;
            
            // Получаем Z и нормаль через MeshIntersectionHelper
            double z = 0.0;
            API_Vector3D normal = { 0.0, 0.0, 1.0 };
            
            if (!MeshIntersectionHelper::GetZAndNormal(xy, z, normal)) {
                Log("[ColumnOrient] failed to get surface normal for column at (%.3f, %.3f)", xy.x, xy.y);
                continue;
            }
            
            // Вычисляем углы наклона из нормали
            double tiltAngle = 0.0;
            double tiltDirection = 0.0;
            ComputeTiltFromNormal(normal, tiltAngle, tiltDirection);
            
            Log("[ColumnOrient] Column %s: normal=(%.3f,%.3f,%.3f) tiltAngle=%.3fdeg tiltDir=%.3fdeg",
                APIGuidToString(colGuid).ToCStr().Get(),
                normal.x, normal.y, normal.z,
                tiltAngle * 180.0 / 3.14159265358979323846,
                tiltDirection * 180.0 / 3.14159265358979323846);
            
            // Устанавливаем ориентацию колонны на основе нормали поверхности
            // axisRotationAngle - поворот вокруг вертикальной оси (Z)
            // Для наклона оси колонны используется другой параметр
            
            API_Element mask{};
            ACAPI_ELEMENT_MASK_CLEAR(mask);
            
            // Сохраняем текущие значения
            const double currentAxisRotation = col.column.axisRotationAngle;
            
            Log("[ColumnOrient] Column current axisRotationAngle=%.3fdeg, tiltAngle=%.3fdeg, tiltDir=%.3fdeg",
                currentAxisRotation * 180.0 / 3.14159265358979323846,
                tiltAngle * 180.0 / 3.14159265358979323846,
                tiltDirection * 180.0 / 3.14159265358979323846);
            
            // У колонн есть параметры для наклона:
            // - isSlanted - булево, наклонена ли колонна
            // - slantAngle - угол наклона (в радианах)
            // - slantDirectionAngle - направление наклона в горизонтальной плоскости (в радианах)
            
            if (tiltAngle > 1e-6) {
                // Колонна наклонена
                col.column.isSlanted = true;
                col.column.slantAngle = tiltAngle;
                col.column.slantDirectionAngle = tiltDirection;
                
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, isSlanted);
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, slantAngle);
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, slantDirectionAngle);
                
                Log("[ColumnOrient] Setting isSlanted=true, slantAngle=%.3fdeg, slantDirectionAngle=%.3fdeg",
                    tiltAngle * 180.0 / 3.14159265358979323846,
                    tiltDirection * 180.0 / 3.14159265358979323846);
            } else {
                // Колонна вертикальная
                col.column.isSlanted = false;
                col.column.slantAngle = 0.0;
                col.column.slantDirectionAngle = 0.0;
                
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, isSlanted);
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, slantAngle);
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, slantDirectionAngle);
                
                Log("[ColumnOrient] Setting isSlanted=false (vertical column)");
            }
            
            // axisRotationAngle - это поворот вокруг оси колонны (не меняем)
            // Оставляем текущее значение axisRotationAngle без изменений
            
            GSErr chg = NoError;
            if (hasMemo) {
                // Используем memo при изменении колонны
                chg = ACAPI_Element_Change(&col, &mask, &memo, 0, true);
                ACAPI_DisposeElemMemoHdls(&memo);
            } else {
                // Изменяем без memo
                chg = ACAPI_Element_Change(&col, &mask, nullptr, 0, true);
            }
            
            if (chg == NoError) {
                Log("[ColumnOrient] SUCCESS: Column %s oriented (axisRotationAngle=%.3fdeg, tiltAngle=%.3fdeg - parameter name needed)",
                    APIGuidToString(colGuid).ToCStr().Get(),
                    tiltDirection * 180.0 / 3.14159265358979323846,
                    tiltAngle * 180.0 / 3.14159265358979323846);
                oriented++;
            } else {
                Log("[ColumnOrient] FAILED: Column %s change error=%d",
                    APIGuidToString(colGuid).ToCStr().Get(), (int)chg);
            }
        }
        
        Log("[ColumnOrient] Oriented %u columns", (unsigned)oriented);
        return NoError;
    });

    Log("[ColumnOrient] OrientColumnsToSurface EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

bool ColumnOrientHelper::OrientBeamsToSurface()
{
    Log("[BeamOrient] OrientBeamsToSurface ENTER");
    
    if (g_beamGuids.IsEmpty()) {
        Log("[BeamOrient] ERR: no beams set");
        return false;
    }
    
    if (g_meshGuid == APINULLGuid) {
        Log("[BeamOrient] ERR: mesh not set, call SetMesh() first");
        return false;
    }

    // Устанавливаем mesh в GroundHelper напрямую по сохранённому GUID
    if (!GroundHelper::SetGroundSurfaceByGuid(g_meshGuid)) {
        Log("[BeamOrient] ERR: failed to set mesh in GroundHelper");
        return false;
    }

    const GSErr cmdErr = ACAPI_CallUndoableCommand("Orient Beams to Surface", [&]() -> GSErr {
        UInt32 oriented = 0;
        
        for (const API_Guid& beamGuid : g_beamGuids) {
            API_Element beam{};
            beam.header.guid = beamGuid;
            if (ACAPI_Element_Get(&beam) != NoError) {
                Log("[BeamOrient] failed to get beam %s", APIGuidToString(beamGuid).ToCStr().Get());
                continue;
            }
            
            if (beam.header.type.typeID != API_BeamID) continue;

            // Получаем начальную точку балки (в плане)
            API_Coord begXY = beam.beam.begC;
            
            // Получаем Z и нормаль поверхности в начальной точке
            double begZ = 0.0;
            API_Vector3D normal = { 0.0, 0.0, 1.0 };
            
            API_Coord3D begPoint3D = { begXY.x, begXY.y, 0.0 };
            if (!GroundHelper::GetGroundZAndNormal(begPoint3D, begZ, normal)) {
                Log("[BeamOrient] failed to get surface normal for beam at (%.3f, %.3f)", begXY.x, begXY.y);
                continue;
            }
            
            Log("[BeamOrient] Beam %s: normal=(%.3f,%.3f,%.3f)",
                APIGuidToString(beamGuid).ToCStr().Get(),
                normal.x, normal.y, normal.z);
            
            // Нормализуем нормаль
            double normalLen = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            if (normalLen < 1e-9) {
                normal = { 0.0, 0.0, 1.0 };
                normalLen = 1.0;
            } else {
                normal.x /= normalLen;
                normal.y /= normalLen;
                normal.z /= normalLen;
            }
            
            // Для балок нужно повернуть балку вокруг её оси так, чтобы она была перпендикулярна нормали
            // Вычисляем текущее направление балки в плане
            const double dx = beam.beam.endC.x - beam.beam.begC.x;
            const double dy = beam.beam.endC.y - beam.beam.begC.y;
            const double beamLength = std::hypot(dx, dy);
            const double currentAngle = std::atan2(dy, dx);
            
            // Вычисляем угол наклона поверхности (угол между нормалью и вертикалью)
            // Этот угол напрямую используется для profileAngle - поворота профиля вокруг оси балки
            double tiltAngle = std::acos(std::max(-1.0, std::min(1.0, normal.z)));
            
            double rotationAngle = 0.0;
            
            // Если нормаль вертикальная (поверхность горизонтальная), профиль не нужно поворачивать
            if (tiltAngle < 0.01) { // меньше ~0.57 градусов - считаем горизонтальной
                rotationAngle = 0.0;
            } else {
                // Используем tiltAngle, но определяем знак на основе направления нормали
                // Проекция нормали на плоскость XY указывает направление наклона
                double normalXYLen = std::hypot(normal.x, normal.y);
                if (normalXYLen < 1e-9) {
                    // Нормаль вертикальная - не нужно поворачивать
                    rotationAngle = 0.0;
                } else {
                    // Направление нормали в плоскости XY
                    double normalDir = std::atan2(normal.y, normal.x);
                    
                    // Стандартное направление профиля (перпендикулярно оси балки)
                    double defaultProfileDir = currentAngle + 1.57079632679489661923; // +90°
                    
                    // Определяем знак на основе угла между направлением нормали и стандартным направлением профиля
                    double angleDiff = normalDir - defaultProfileDir;
                    
                    // Нормализуем разницу углов в диапазон [-PI, PI]
                    while (angleDiff > 3.14159265358979323846) angleDiff -= 2.0 * 3.14159265358979323846;
                    while (angleDiff < -3.14159265358979323846) angleDiff += 2.0 * 3.14159265358979323846;
                    
                    // Используем tiltAngle с тем же знаком, что и angleDiff
                    rotationAngle = (angleDiff >= 0.0) ? tiltAngle : -tiltAngle;
                }
            }
            
            Log("[BeamOrient] Beam %s: beg=(%.3f,%.3f) end=(%.3f,%.3f) currentAngle=%.3fdeg",
                APIGuidToString(beamGuid).ToCStr().Get(),
                beam.beam.begC.x, beam.beam.begC.y,
                beam.beam.endC.x, beam.beam.endC.y,
                currentAngle * 180.0 / 3.14159265358979323846);
            
            if (tiltAngle < 0.01) {
                Log("[BeamOrient] Beam %s: normal=(%.3f,%.3f,%.3f) tiltAngle=%.3fdeg -> horizontal surface, rotationAngle=0.0deg",
                    APIGuidToString(beamGuid).ToCStr().Get(),
                    normal.x, normal.y, normal.z,
                    tiltAngle * 180.0 / 3.14159265358979323846);
            } else {
                double normalXYLen = std::hypot(normal.x, normal.y);
                if (normalXYLen > 1e-9) {
                    double normalDir = std::atan2(normal.y, normal.x);
                    double defaultProfileDir = currentAngle + 1.57079632679489661923;
                    double angleDiff = normalDir - defaultProfileDir;
                    // Нормализуем для логирования
                    while (angleDiff > 3.14159265358979323846) angleDiff -= 2.0 * 3.14159265358979323846;
                    while (angleDiff < -3.14159265358979323846) angleDiff += 2.0 * 3.14159265358979323846;
                    Log("[BeamOrient] Beam %s: normal=(%.3f,%.3f,%.3f) tiltAngle=%.3fdeg, normalDir=%.3fdeg, defaultProfileDir=%.3fdeg, angleDiff=%.3fdeg, rotationAngle=%.3fdeg",
                        APIGuidToString(beamGuid).ToCStr().Get(),
                        normal.x, normal.y, normal.z,
                        tiltAngle * 180.0 / 3.14159265358979323846,
                        normalDir * 180.0 / 3.14159265358979323846,
                        defaultProfileDir * 180.0 / 3.14159265358979323846,
                        angleDiff * 180.0 / 3.14159265358979323846,
                        rotationAngle * 180.0 / 3.14159265358979323846);
                } else {
                    Log("[BeamOrient] Beam %s: normal=(%.3f,%.3f,%.3f) tiltAngle=%.3fdeg -> vertical normal, rotationAngle=0.0deg",
                        APIGuidToString(beamGuid).ToCStr().Get(),
                        normal.x, normal.y, normal.z,
                        tiltAngle * 180.0 / 3.14159265358979323846);
                }
            }
            
            API_Element mask{};
            ACAPI_ELEMENT_MASK_CLEAR(mask);
            
            // Устанавливаем угол поворота балки вокруг её оси
            // У балок, вероятно, есть поле rotationAngle или подобное
            // Пока пробуем использовать доступные параметры
            // TODO: Найти правильное имя параметра для поворота балки вокруг оси
            
            // Устанавливаем угол поворота балки вокруг её оси
            // Для балок используется поле profileAngle - угол поворота профиля вокруг центральной линии
            // Используем rotationAngle (поворот вокруг оси) для profileAngle
            beam.beam.profileAngle = rotationAngle;
            ACAPI_ELEMENT_MASK_SET(mask, API_BeamType, profileAngle);
            
            Log("[BeamOrient] Setting beam profileAngle=%.3fdeg (rotation around beam center line)",
                rotationAngle * 180.0 / 3.14159265358979323846);
            
            // Загружаем memo балки
            API_ElementMemo memo{};
            bool hasMemo = (ACAPI_Element_GetMemo(beamGuid, &memo, APIMemoMask_All) == NoError);
            
            GSErr chg = NoError;
            if (hasMemo) {
                chg = ACAPI_Element_Change(&beam, &mask, &memo, 0, true);
                ACAPI_DisposeElemMemoHdls(&memo);
            } else {
                chg = ACAPI_Element_Change(&beam, &mask, nullptr, 0, true);
            }
            
            if (chg == NoError) {
                Log("[BeamOrient] SUCCESS: Beam %s oriented", APIGuidToString(beamGuid).ToCStr().Get());
                oriented++;
            } else {
                Log("[BeamOrient] FAILED: Beam %s change error=%d",
                    APIGuidToString(beamGuid).ToCStr().Get(), (int)chg);
            }
        }
        
        Log("[BeamOrient] Oriented %u beams", (unsigned)oriented);
        return NoError;
    });

    Log("[BeamOrient] OrientBeamsToSurface EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

// ================================================================
// Поворот выделенных балок на заданный угол
// ================================================================
bool ColumnOrientHelper::RotateSelected(double angleDeg)
{
    Log("[RotateOrient] RotateSelected ENTER angleDeg=%.3f", angleDeg);
    
    if (std::abs(angleDeg) < 1e-6) {
        Log("[RotateOrient] Angle is zero, nothing to do");
        return true;
    }
    
    API_SelectionInfo selInfo{};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selInfo.marquee.coords);
    
    if (selNeigs.IsEmpty()) {
        Log("[RotateOrient] No selection");
        return false;
    }
    
    const double angleRad = angleDeg * 3.14159265358979323846 / 180.0;
    
    GSErrCode cmdErr = ACAPI_CallUndoableCommand("Rotate Selected Orientation", [&]() -> GSErrCode {
        unsigned rotated = 0;
        
        for (const API_Neig& n : selNeigs) {
            API_Element element{};
            element.header.guid = n.guid;
            if (ACAPI_Element_Get(&element) != NoError) continue;
            
            API_Element mask{};
            ACAPI_ELEMENT_MASK_CLEAR(mask);
            
            API_ElementMemo memo{};
            bool needsMemo = false;
            bool hasMemo = false;
            bool changed = false;
            
            switch (element.header.type.typeID) {
            case API_BeamID:
                element.beam.profileAngle += angleRad;
                ACAPI_ELEMENT_MASK_SET(mask, API_BeamType, profileAngle);
                needsMemo = true;
                hasMemo = (ACAPI_Element_GetMemo(n.guid, &memo, APIMemoMask_All) == NoError);
                changed = true;
                Log("[RotateOrient] Beam %s: added %.3fdeg to profileAngle",
                    APIGuidToString(n.guid).ToCStr().Get(), angleDeg);
                break;
                
            default:
                continue;
            }
            
            if (changed) {
                GSErrCode chg = NoError;
                if (needsMemo && hasMemo) {
                    chg = ACAPI_Element_Change(&element, &mask, &memo, 0, true);
                    ACAPI_DisposeElemMemoHdls(&memo);
                } else {
                    chg = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
                }
                
                if (chg == NoError) {
                    rotated++;
                } else {
                    Log("[RotateOrient] FAILED to change element %s: error=%d",
                        APIGuidToString(n.guid).ToCStr().Get(), (int)chg);
                }
            }
        }
        
        Log("[RotateOrient] Rotated %u elements", rotated);
        return NoError;
    });
    
    Log("[RotateOrient] RotateSelected EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

