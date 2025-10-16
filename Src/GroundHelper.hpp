#ifndef GROUNDHELPER_HPP
#define GROUNDHELPER_HPP

#include "APIdefs_Elements.h"
#include "API_Guid.hpp"

class GroundHelper {
public:
    // выбрать поверхность (Mesh) из текущего выделения
    static bool SetGroundSurface();

    // собрать GUID’ы объектов для "посадки" из текущего выделения (Object/Lamp/Column)
    static bool SetGroundObjects();

    // посадить выбранные объекты на поверхность (с офсетом по Z)
    static bool ApplyGroundOffset(double offset);

    // получить Z и нормаль поверхности в XY-точке
    static bool GetGroundZAndNormal(const API_Coord3D& pos3D, double& z, API_Vector3D& normal);

    // быстрая отладка одного выбранного элемента
    static bool DebugOneSelection();
};

#endif // GROUNDHELPER_HPP