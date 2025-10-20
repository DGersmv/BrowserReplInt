#ifndef GROUNDHELPER_HPP
#define GROUNDHELPER_HPP

#include "APIdefs_Elements.h"
#include "API_Guid.hpp"

class GroundHelper {
public:
    static bool SetGroundSurface();
    static bool SetGroundObjects();
    static bool GetGroundZAndNormal(const API_Coord3D& pos3D, double& z, API_Vector3D& normal);

    // Приземлить на mesh (offset игнорируется, ставим ровно на поверхность)
    static bool ApplyGroundOffset(double /*offset*/);

    // Смещение по Z без mesh/TIN
    static bool ApplyZDelta(double deltaMeters);

    static bool DebugOneSelection();
};

#endif // GROUNDHELPER_HPP
