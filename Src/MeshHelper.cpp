// ============================================================================
// MeshHelper.cpp — создание Mesh элементов
// ============================================================================

#include "MeshHelper.hpp"
#include "BrowserRepl.hpp"
#include "ACAPinc.h"
#include "APICommon.h"

#include <cstdio>
#include <cstdarg>

// ----------------- Logging -----------------
static inline void Log(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    if (BrowserRepl::HasInstance()) {
        BrowserRepl::GetInstance().LogToBrowser(GS::UniString(buf));
    }
    ACAPI_WriteReport(buf, false);
}

// ============================================================================
// CreateMesh — создание Mesh элемента из точек, указанных пользователем
// Использует CreateMeshFromPoints для универсальной обработки
// ============================================================================
bool MeshHelper::CreateMesh()
{
    Log("[MeshHelper] CreateMesh: START - запрашиваем точки у пользователя");
    
    // ВАЖНО: Весь пользовательский ввод и создание mesh оборачиваем в одну Undo-команду
    // Это гарантирует правильный контекст выполнения и предотвращает падения
    bool success = false;
    GSErrCode err = ACAPI_CallUndoableCommand("Create Mesh from Points", [&]() -> GSErrCode {
        GS::Array<API_Coord3D> points;
        Int32 pointNum = 1;
        
        Log("[MeshHelper] Запрашиваем точки у пользователя");
        
        // Минимум 3 точки нужно получить обязательно
        while (pointNum <= 3) {
            API_GetPointType gp = {};
            char promptBuf[256];
            std::sprintf(promptBuf, "Mesh: укажите точку %d/3", pointNum);
            CHTruncate(promptBuf, gp.prompt, sizeof(gp.prompt));
            gp.changeFilter = false;
            gp.changePlane = false;
            
            Log("[MeshHelper] Запрашиваем точку %d...", pointNum);
            GSErrCode inputErr = ACAPI_UserInput_GetPoint(&gp);
            
            if (inputErr != NoError) {
                Log("[MeshHelper] Отменено или ошибка при вводе точки %d, err=%d", pointNum, (int)inputErr);
                return inputErr; // Отменяем всю команду
            }
            
            Log("[MeshHelper] Точка %d получена: (%.3f, %.3f, %.3f)", 
                pointNum, gp.pos.x, gp.pos.y, gp.pos.z);
            
            points.Push(API_Coord3D{gp.pos.x, gp.pos.y, gp.pos.z});
            pointNum++;
        }
        
        Log("[MeshHelper] Получено 3 точки, создаем mesh");
        
        if (points.GetSize() < 3) {
            Log("[MeshHelper] ERROR: Недостаточно точек");
            return APIERR_BADPOLY;
        }
        
        // Создаем mesh из полученных точек
        // CreateMeshFromPoints уже внутри Undo-команды, но нам нужно вызвать её напрямую
        // без дополнительной обертки
        success = CreateMeshFromPointsInternal(points);
        if (!success) {
            Log("[MeshHelper] ERROR: CreateMeshFromPointsInternal failed");
            return APIERR_BADPOLY;
        }
        
        return NoError;
    });
    
    if (err == NoError && success) {
        Log("[MeshHelper] SUCCESS: Mesh создан");
        return true;
    } else {
        Log("[MeshHelper] ERROR: Не удалось создать Mesh, err=%d", (int)err);
        return false;
    }
}

// ============================================================================
// CreateMeshFromPoints — создание Mesh из произвольного количества точек
// ============================================================================
bool MeshHelper::CreateMeshFromPoints(const GS::Array<API_Coord3D>& points)
{
    return ACAPI_CallUndoableCommand("Create Mesh from Points", [&]() -> GSErrCode {
        if (CreateMeshFromPointsInternal(points)) {
            return NoError;
        } else {
            return APIERR_BADPOLY;
        }
    }) == NoError;
}

// ============================================================================
// CreateMeshFromPointsInternal — внутренняя реализация без Undo-обертки
// ============================================================================
bool MeshHelper::CreateMeshFromPointsInternal(const GS::Array<API_Coord3D>& points)
{
    const UIndex numPoints = points.GetSize();
    Log("[MeshHelper] CreateMeshFromPoints: START с %d точками", (int)numPoints);
    
    if (numPoints < 3) {
        Log("[MeshHelper] ERROR: Нужно минимум 3 точки для создания mesh");
        return false;
    }
    
    // Создаем Mesh элемент
    API_Element element = {};
    element.header.type = API_MeshID;
    
    GSErrCode err = ACAPI_Element_GetDefaults(&element, nullptr);
    if (err != NoError) {
        Log("[MeshHelper] ERROR: ACAPI_Element_GetDefaults failed, err=%d", (int)err);
        return false;
    }
    
    // Определяем стратегию:
    // - Если точек <= 6: все точки как контур
    // - Если точек > 6: первые 4-6 точек как контур, остальные как meshLevelCoords
    const UIndex contourPoints = (numPoints <= 6) ? numPoints : 6;
    const UIndex levelPoints = (numPoints > 6) ? (numPoints - contourPoints) : 0;
    
    Log("[MeshHelper] Стратегия: %d точек контура, %d точек уровня", (int)contourPoints, (int)levelPoints);
    
    // Настройки контура
    element.mesh.poly.nCoords = contourPoints;
    element.mesh.poly.nSubPolys = 1;
    element.mesh.poly.nArcs = 0;
    
    Log("[MeshHelper] element.mesh настройки:");
    Log("[MeshHelper]   poly.nCoords = %d", (int)element.mesh.poly.nCoords);
    Log("[MeshHelper]   poly.nSubPolys = %d", (int)element.mesh.poly.nSubPolys);
    Log("[MeshHelper]   poly.nArcs = %d", (int)element.mesh.poly.nArcs);
    Log("[MeshHelper]   level = %.3f", element.mesh.level);
    Log("[MeshHelper]   floorInd = %d", (int)element.header.floorInd);
    
    // Создаем memo для Mesh
    API_ElementMemo memo = {};
    memo.coords = reinterpret_cast<API_Coord**>(BMAllocateHandle((element.mesh.poly.nCoords + 1) * sizeof(API_Coord), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**>(BMAllocateHandle((element.mesh.poly.nSubPolys + 1) * sizeof(Int32), ALLOCATE_CLEAR, 0));
    memo.parcs = reinterpret_cast<API_PolyArc**>(BMAllocateHandle(element.mesh.poly.nArcs * sizeof(API_PolyArc), ALLOCATE_CLEAR, 0));
    memo.meshPolyZ = reinterpret_cast<double**>(BMAllocateHandle((element.mesh.poly.nCoords + 1) * sizeof(double), ALLOCATE_CLEAR, 0));
    
    if (!memo.coords || !memo.pends || !memo.meshPolyZ) {
        Log("[MeshHelper] ERROR: Failed to allocate memory for Mesh memo");
        ACAPI_DisposeElemMemoHdls(&memo);
        return false;
    }
    
    // Заполняем контурные координаты (XY) и Z-координаты
    // В официальном примере: точки заполняются от 1 до nCoords-1, затем замыкание на nCoords
    Log("[MeshHelper] Заполняем координаты контура (%d точек):", (int)contourPoints);
    for (UIndex i = 0; i < contourPoints; ++i) {
        (*memo.coords)[i + 1].x = points[i].x;
        (*memo.coords)[i + 1].y = points[i].y;
        (*memo.meshPolyZ)[i + 1] = points[i].z;
        Log("[MeshHelper]   coords[%d]: XY=(%.3f, %.3f) Z=%.3f", 
            (int)(i + 1), points[i].x, points[i].y, points[i].z);
    }
    // Замыкаем контур - ТОЧНО как в официальном примере Element_Test
    // Пример показывает: (*memo.coords)[element.mesh.poly.nCoords] = (*memo.coords)[1];
    // Для 3 точек: nCoords=3, заполняем индексы 1,2,3, затем замыкание на индекс 4 (nCoords+1)
    // Но пример использует индекс nCoords для замыкания!
    (*memo.coords)[element.mesh.poly.nCoords + 1].x = (*memo.coords)[1].x;
    (*memo.coords)[element.mesh.poly.nCoords + 1].y = (*memo.coords)[1].y;
    (*memo.meshPolyZ)[element.mesh.poly.nCoords + 1] = (*memo.meshPolyZ)[1];
    Log("[MeshHelper]   coords[%d]: XY=(%.3f, %.3f) Z=%.3f <- замыкание", 
        (int)(element.mesh.poly.nCoords + 1), 
        (*memo.coords)[element.mesh.poly.nCoords + 1].x,
        (*memo.coords)[element.mesh.poly.nCoords + 1].y,
        (*memo.meshPolyZ)[element.mesh.poly.nCoords + 1]);
    
    (*memo.pends)[1] = element.mesh.poly.nCoords;
    Log("[MeshHelper] pends[1] = %d", (int)(*memo.pends)[1]);
    
    // Если есть дополнительные точки, используем их как meshLevelCoords
    if (levelPoints > 0) {
        Log("[MeshHelper] Создаем meshLevelCoords для %d дополнительных точек", (int)levelPoints);
        
        // Получаем базовый Z правильно: storyZ + mesh.level (как в GroundHelper)
        double storyZ = 0.0;
        API_StoryInfo si = {};
        if (ACAPI_ProjectSetting_GetStorySettings(&si) == NoError && si.data != nullptr) {
            const Int32 floorInd = element.header.floorInd;
            const Int32 cnt = (Int32)(BMGetHandleSize((GSHandle)si.data) / sizeof(API_StoryType));
            if (floorInd >= 0 && cnt > 0) {
                const Int32 idx = floorInd - si.firstStory;
                if (0 <= idx && idx < cnt) {
                    storyZ = (*si.data)[idx].level;
                }
            }
            BMKillHandle((GSHandle*)&si.data);
        }
        const double baseZ = storyZ + element.mesh.level;
        
        Log("[MeshHelper] Базовый Z для meshLevelCoords: storyZ=%.3f + mesh.level=%.3f = %.3f", 
            storyZ, element.mesh.level, baseZ);
        
        memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**>(
            BMAllocateHandle(levelPoints * sizeof(API_MeshLevelCoord), ALLOCATE_CLEAR, 0));
        
        if (memo.meshLevelCoords != nullptr) {
            for (UIndex i = 0; i < levelPoints; ++i) {
                const UIndex pointIdx = contourPoints + i;
                (*memo.meshLevelCoords)[i].c.x = points[pointIdx].x;
                (*memo.meshLevelCoords)[i].c.y = points[pointIdx].y;
                // Z в meshLevelCoords это относительное смещение от базового уровня
                (*memo.meshLevelCoords)[i].c.z = points[pointIdx].z - baseZ;
                
                if (i < 5 || levelPoints <= 10) {
                    Log("[MeshHelper]   level[%d]: XY=(%.3f, %.3f) absZ=%.3f -> relZ=%.3f", 
                        (int)i, 
                        (*memo.meshLevelCoords)[i].c.x,
                        (*memo.meshLevelCoords)[i].c.y,
                        points[pointIdx].z,
                        (*memo.meshLevelCoords)[i].c.z);
                }
            }
            if (levelPoints > 10) {
                Log("[MeshHelper] ... и еще %d точек уровня", (int)(levelPoints - 10));
            }
        } else {
            Log("[MeshHelper] WARNING: Не удалось выделить память для meshLevelCoords");
        }
    }
    
    Log("[MeshHelper] Заполнены координаты контура и meshLevelCoords");
    Log("[MeshHelper] Параметры: контур=%d точек, meshLevelCoords=%d точек", (int)contourPoints, (int)levelPoints);
    
    // Создаем Mesh (без Undo-обертки, т.к. она уже есть в вызывающем коде)
    GSErrCode createErr = ACAPI_Element_Create(&element, &memo);
    
    Log("[MeshHelper] ACAPI_Element_Create вернул err=%d", (int)createErr);
    
    if (createErr != NoError && createErr != APIERR_IRREGULARPOLY) {
        Log("[MeshHelper] ERROR: Неожиданная ошибка при создании mesh, err=%d", (int)createErr);
        // Выводим детальную информацию об ошибке
        if (createErr == APIERR_BADPOLY) {
            Log("[MeshHelper] ERROR: APIERR_BADPOLY - плохой полигон (возможно коллинеарные точки)");
        }
    }
    
    // Если полигон нерегулярный, регуляризуем
    if (createErr == APIERR_IRREGULARPOLY) {
        Log("[MeshHelper] Полигон нерегулярный, регуляризуем...");
        
        API_RegularizedPoly poly = {};
        poly.coords = memo.coords;
        poly.pends = memo.pends;
        poly.parcs = memo.parcs;
        poly.vertexIDs = memo.vertexIDs;
        poly.needVertexAncestry = 1;
        
        Int32 nResult = 0;
        API_RegularizedPoly** polys = nullptr;
        GSErrCode regErr = ACAPI_Polygon_RegularizePolygon(&poly, &nResult, &polys);
        
        if (regErr != NoError) {
            Log("[MeshHelper] ERROR: ACAPI_Polygon_RegularizePolygon failed, err=%d", (int)regErr);
            ACAPI_DisposeElemMemoHdls(&memo);
            return false;
        }
        
        if (regErr == NoError && nResult > 0) {
            Log("[MeshHelper] Регуляризация успешна, создаем %d полигонов", (int)nResult);
            Log("[MeshHelper] ВАЖНО: meshLevelCoords не передаются в регуляризованные полигоны");
            
            for (Int32 i = 0; i < nResult && createErr == NoError; i++) {
                element.mesh.poly.nCoords = BMhGetSize(reinterpret_cast<GSHandle>((*polys)[i].coords)) / sizeof(API_Coord) - 1;
                element.mesh.poly.nSubPolys = BMhGetSize(reinterpret_cast<GSHandle>((*polys)[i].pends)) / sizeof(Int32) - 1;
                element.mesh.poly.nArcs = BMhGetSize(reinterpret_cast<GSHandle>((*polys)[i].parcs)) / sizeof(API_PolyArc);
                
                API_ElementMemo tmpMemo = {};
                tmpMemo.coords = (*polys)[i].coords;
                tmpMemo.pends = (*polys)[i].pends;
                tmpMemo.parcs = (*polys)[i].parcs;
                tmpMemo.vertexIDs = (*polys)[i].vertexIDs;
                
                // НЕ копируем meshLevelCoords в регуляризованные полигоны
                // Это может вызывать падение
                
                tmpMemo.meshPolyZ = reinterpret_cast<double**>(BMAllocateHandle((element.mesh.poly.nCoords + 1) * sizeof(double), ALLOCATE_CLEAR, 0));
                if (tmpMemo.meshPolyZ != nullptr) {
                    // Используем vertexAncestry для правильного копирования Z
                    if ((*polys)[i].vertexAncestry != nullptr && memo.meshPolyZ != nullptr) {
                        for (Int32 j = 1; j <= element.mesh.poly.nCoords; j++) {
                            Int32 oldVertexIndex = (*(*polys)[i].vertexAncestry)[j];
                            if (oldVertexIndex == 0) {
                                // новая вершина после регуляризации
                                double avgZ = 0.0;
                                for (UIndex k = 0; k < contourPoints; ++k) {
                                    avgZ += points[k].z;
                                }
                                avgZ /= contourPoints;
                                (*tmpMemo.meshPolyZ)[j] = avgZ;
                            } else if (oldVertexIndex >= 1 && oldVertexIndex <= (Int32)contourPoints) {
                                // используем Z из исходной вершины
                                (*tmpMemo.meshPolyZ)[j] = (*memo.meshPolyZ)[oldVertexIndex];
                            } else {
                                (*tmpMemo.meshPolyZ)[j] = 0.0;
                            }
                        }
                    } else {
                        // Fallback: используем среднее Z
                        double avgZ = 0.0;
                        for (UIndex j = 0; j < contourPoints; ++j) {
                            avgZ += points[j].z;
                        }
                        avgZ /= contourPoints;
                        for (Int32 j = 1; j <= element.mesh.poly.nCoords; j++) {
                            (*tmpMemo.meshPolyZ)[j] = avgZ;
                        }
                    }
                    (*tmpMemo.meshPolyZ)[element.mesh.poly.nCoords + 1] = (*tmpMemo.meshPolyZ)[1];
                    
                    createErr = ACAPI_Element_Create(&element, &tmpMemo);
                    if (createErr != NoError && createErr != APIERR_IRREGULARPOLY) {
                        Log("[MeshHelper] ERROR: ACAPI_Element_Create piece %d failed, err=%d", (int)i, (int)createErr);
                    }
                    BMKillHandle(reinterpret_cast<GSHandle*>(&tmpMemo.meshPolyZ));
                } else {
                    Log("[MeshHelper] ERROR: Failed to allocate meshPolyZ for piece %d", (int)i);
                    createErr = APIERR_MEMFULL;
                }
            }
            
            // Освобождаем память регуляризованных полигонов
            for (Int32 j = 0; j < nResult; ++j) {
                ACAPI_Polygon_DisposeRegularizedPoly(&(*polys)[j]);
            }
            BMKillHandle(reinterpret_cast<GSHandle*>(&polys));
        }
    }
        
    ACAPI_DisposeElemMemoHdls(&memo);
    
    if (createErr == NoError) {
        Log("[MeshHelper] SUCCESS: Mesh создан из %d точек (контур: %d, уровень: %d)", 
            (int)numPoints, (int)contourPoints, (int)levelPoints);
        return true;
    } else {
        Log("[MeshHelper] ERROR: Не удалось создать Mesh, err=%d", (int)createErr);
        return false;
    }
}

