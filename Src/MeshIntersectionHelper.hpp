#ifndef MESHINTERSECTIONHELPER_HPP
#define MESHINTERSECTIONHELPER_HPP

#include "APIdefs_3D.h"

// ============================================================================
// MeshIntersectionHelper — пересечение с Mesh поверхностью через TIN
// Получает Z координату и нормаль поверхности по координатам XY
// ============================================================================
class MeshIntersectionHelper {
public:
    // Получить Z координату и нормаль поверхности в точке
    // xy: входная точка с координатами XY (z игнорируется)
    // outZ: выходная абсолютная Z координата поверхности
    // outNormal: выходная нормаль поверхности в точке (единичный вектор)
    // Возвращает true если пересечение найдено
    static bool GetZAndNormal(const API_Coord& xy, double& outZ, API_Vector3D& outNormal);
};

#endif // MESHINTERSECTIONHELPER_HPP

