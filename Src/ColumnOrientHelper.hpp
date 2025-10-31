#ifndef COLUMNORIENTHELPER_HPP
#define COLUMNORIENTHELPER_HPP

#include "API_Guid.hpp"

// ============================================================================
// ColumnOrientHelper — ориентация колонн по поверхности mesh
// ============================================================================
class ColumnOrientHelper {
public:
    // Установить колонны для ориентации (из выделения)
    static bool SetColumns();
    
    // Установить mesh для ориентации (из выделения)
    static bool SetMesh();
    
    // Ориентировать колонны по поверхности mesh (не меняя высоту колонн)
    static bool OrientColumnsToSurface();
};

#endif // COLUMNORIENTHELPER_HPP

