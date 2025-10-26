#ifndef MARKUPHELPER_HPP
#define MARKUPHELPER_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace MarkupHelper {

    // Установить шаг разметки (в миллиметрах из UI)
    bool SetMarkupStep(double stepMM);

    // Создать разметку размерами (вызывает GetLine для направления)
    bool CreateMarkupDimensions();

    // Проставить размеры от точек привязки объектов до линии (перпендикулярно)
    bool CreateDimensionsToLine();

    // Проставить размеры между объектами последовательно (1→2, 2→3, 3→4...)
    bool CreateDimensionsBetweenObjects();

    // Проставить размеры от объектов до выбранной точки
    bool CreateDimensionsToPoint();

} // namespace MarkupHelper

#endif // MARKUPHELPER_HPP


