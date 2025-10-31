#ifndef MESHHELPER_HPP
#define MESHHELPER_HPP
#pragma once

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "APIdefs_Elements.h"
#include "GSRoot.hpp"

// ============================================================================
// MeshHelper — создание Mesh элементов
// ============================================================================
class MeshHelper {
public:
    // Создать простой Mesh элемент из 3 точек (треугольник)
    static bool CreateMesh();
    
    // Создать Mesh из произвольного количества точек
    // Если точек <= 6: все как контур
    // Если точек > 6: первые 4-6 как контур, остальные как meshLevelCoords
    static bool CreateMeshFromPoints(const GS::Array<API_Coord3D>& points);
    
private:
    // Внутренняя реализация без Undo-обертки (для использования внутри существующей Undo-команды)
    static bool CreateMeshFromPointsInternal(const GS::Array<API_Coord3D>& points);
};

#endif // MESHHELPER_HPP

