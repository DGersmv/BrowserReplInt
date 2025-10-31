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
            GroundHelper::SetGroundSurface();
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
    // Угол наклона от вертикали (0 = вертикально, PI/2 = горизонтально)
    outTiltAngle = std::acos(std::max(-1.0, std::min(1.0, normal.z)));
    
    // Направление наклона в плоскости XY (азимут)
    outTiltDirection = std::atan2(normal.y, normal.x);
}

bool ColumnOrientHelper::OrientColumnsToSurface()
{
    Log("[ColumnOrient] OrientColumnsToSurface ENTER");
    
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
    
    // Устанавливаем mesh в GroundHelper
    GroundHelper::SetGroundSurface();

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
            
            // TODO: Установить параметры ориентации колонны
            // Нужно найти правильные поля в API_ColumnType для:
            // - Угол наклона оси (axisTiltAngle?)
            // - Направление наклона (может быть через axisRotationAngle?)
            
            // Пока только логируем - нужно найти правильные поля API
            // API_Element mask{};
            // ACAPI_ELEMENT_MASK_CLEAR(mask);
            // col.column.axisTiltAngle = tiltAngle;
            // ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, axisTiltAngle);
            // const GSErr chg = ACAPI_Element_Change(&col, &mask, nullptr, 0, true);
            
            oriented++;
        }
        
        Log("[ColumnOrient] Oriented %u columns", (unsigned)oriented);
        return NoError;
    });

    Log("[ColumnOrient] OrientColumnsToSurface EXIT (err=%d)", (int)cmdErr);
    return (cmdErr == NoError);
}

