#ifndef LANDSCAPEHELPER_HPP
#define LANDSCAPEHELPER_HPP

#pragma once
#include "APIEnvir.h"
#include "ACAPinc.h"

namespace LandscapeHelper {

	// Выбрать линии/дуги/окружности/полилинии/сплайны из текущего выделения как пути (несколько!)
	bool SetDistributionLine();

	// Выбрать "прототип" (Object/Lamp/Column) из выделения
	bool SetDistributionObject();

	// Задать шаг (плановые единицы проекта; >0 активирует режим шага)
	bool SetDistributionStep(double step);

	// Задать количество (>=1 активирует режим количества; перекрывает шаг)
	bool SetDistributionCount(int count);

	// Выполнить раскладку (если step/count переданы - перекрывают сохранённые)
	bool DistributeSelected(double step, int count);

} // namespace LandscapeHelper

#endif // LANDSCAPEHELPER_HPP
