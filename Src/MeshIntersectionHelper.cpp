// ============================================================================
// MeshIntersectionHelper.cpp — пересечение с Mesh поверхностью через TIN
// Получает Z координату и нормаль поверхности по координатам XY
// ============================================================================

#include "MeshIntersectionHelper.hpp"
#include "GroundHelper.hpp"

bool MeshIntersectionHelper::GetZAndNormal(const API_Coord& xy, double& outZ, API_Vector3D& outNormal)
{
    // Используем существующий функционал из GroundHelper
    // Преобразуем API_Coord в API_Coord3D (z = 0, т.к. игнорируется)
    API_Coord3D pos3D = { xy.x, xy.y, 0.0 };
    
    // Вызываем GroundHelper, который использует mesh, выбранный через SetGroundSurface()
    return GroundHelper::GetGroundZAndNormal(pos3D, outZ, outNormal);
}

