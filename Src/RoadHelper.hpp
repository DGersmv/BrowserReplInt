#pragma once

#include "ACAPinc.h"
#include "APIdefs_Elements.h"
#include "GSRoot.hpp"
#include "Array.hpp"

// RoadHelper
// Генерация дорожки/ленты по рельефу:
// - осевая линия (Spline / PolyLine / Arc / Line)
// - поверхность рельефа (Mesh)
// - ширина дорожки
// - точность профиля (шаг замера вдоль оси)
// Результат:
// - Morph по рельефу (3D тело)
// - Hatch (штриховка) в плане по границам
// - Текст "Расчётная площадь: NN.N м²"
// - Логирование через BrowserRepl

namespace RoadHelper {

	// Пользовательские параметры генерации
	struct RoadParams {
		double widthMM;         // ширина дорожки в мм
		double sampleStepMM;    // шаг съёма профиля вдоль осевой в мм (точность профиля)
	};

	// Одна осевая линия, которую выбрал пользователь
	bool SetCenterLine ();          // сохраняет GUID выбранной линии
	bool SetTerrainMesh ();         // сохраняет GUID выбранной Mesh

	// Основная команда
	bool BuildRoad (const RoadParams& params);

	// Вспомогательные функции для построения перпендикуляров
	bool BuildPerpendicularPoints(const GS::Array<API_Coord>& centerPts, double halfWidthM, 
	                             GS::Array<API_Coord>& leftPts, GS::Array<API_Coord>& rightPts);

	// ------------------------------------------------------------------
	// Внутренние структуры (чтобы .cpp мог работать понятно)
	// ------------------------------------------------------------------

	struct SampledSectionPoint {
		API_Coord3D leftPt;
		API_Coord3D rightPt;
	};

	// Сырые точки с шагом по осевой линии (до сглаживания)
	struct RawProfile {
		GS::Array<API_Coord3D> leftSide;   // точки левого края (в 3D с z от рельефа)
		GS::Array<API_Coord3D> rightSide;  // точки правого края
	};

	// Сглаженные кривые после фитинга через сплайн и повторной дискретизации
	struct SmoothProfile {
		GS::Array<API_Coord3D> leftSide;   // сглаженный левый край (плотный)
		GS::Array<API_Coord3D> rightSide;  // сглаженный правый край (плотный)
	};

} // namespace RoadHelper
