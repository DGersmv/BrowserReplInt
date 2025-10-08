#ifndef LANDSCAPEHELPER_HPP
#define LANDSCAPEHELPER_HPP

#pragma once
#include "APIEnvir.h"
#include "ACAPinc.h"

namespace LandscapeHelper {
	// Выбрать линию/полилинию/дугу/сплайн из текущего выделения как путь
	bool SetDistributionLine();

	// Выбрать "прототипный" объект (API_ObjectID или API_LampID) из выделения
	bool SetDistributionObject();

	// Задать фиксированный шаг (если >0, приоритет над count)
	bool SetDistributionStep(double step);

	// Задать количество (если step<=0)
	bool SetDistributionCount(int count);

	// Выполнить распределение (если step/count передан не 0, они перекрывают сохранённые)
	bool DistributeSelected(double step, int count);
}


#endif // LANDSCAPEHELPER_HPP
