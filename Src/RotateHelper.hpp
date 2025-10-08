#ifndef ROTATEHELPER_HPP
#define ROTATEHELPER_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "GSRoot.hpp"

namespace RotateHelper {

    // Повернуть выделенные элементы на угол (в градусах)
    bool RotateSelected (double angleDeg);

    // Выравнивание по оси X (угол = 0)
    bool AlignSelectedX ();

    // Случайные углы для выделенных
    bool RandomizeSelectedAngles ();

    // Сориентировать выделенные на выбранную точку
    bool OrientObjectsToPoint ();

}

#endif // ROTATEHELPER_HPP
