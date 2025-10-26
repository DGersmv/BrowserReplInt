#ifndef SHELLHELPER_HPP
#define SHELLHELPER_HPP
#pragma once

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "APIdefs_Elements.h"
#include "GSRoot.hpp"

namespace ShellHelper {

	// ----------------- Глобальные GUID-ы -----------------
	extern API_Guid g_baseLineGuid;      // GUID базовой линии
	extern API_Guid g_meshSurfaceGuid;   // GUID Mesh поверхности

	// ----------------- Типы сегментов пути ----------------
	enum class SegType { Line, Arc, Cubic };

	struct Seg {
		SegType   type = SegType::Line;

		// Line
		API_Coord A{}, B{};

		// Arc
		API_Coord C{}; double r = 0.0;
		double    a0 = 0.0, a1 = 0.0;
		bool      ccw = true;

		// Общая длина
		double    len = 0.0;
	};

	struct PathData {
		GS::Array<Seg> segs;
		double total = 0.0;
	};

	// ----------------- Публичные функции ------------------

	// Выбор базовой линии
	bool SetBaseLineForShell();

	// Создать простую оболочку и/или тестовый mesh
	bool CreateShellFromLine(double widthMM, double stepMM);
	bool CreateTestMesh();
	bool CreateSimpleShell();

	// Анализ базовой линии и генерация перпендикуляров/проекция на mesh
	GS::Array<API_Coord3D> AnalyzeBaseLine(const API_Guid& lineGuid, double stepMM);
	GS::Array<API_Coord3D> GeneratePerpendicularLines(const GS::Array<API_Coord3D>& basePoints, double widthMM);
	GS::Array<API_Coord3D> ProjectToMesh(const GS::Array<API_Coord3D>& points);
	bool CreateShellGeometry(const GS::Array<API_Coord3D>& shellPoints);
	bool CreatePerpendicularLines(const API_Element& baseLine, double widthMM);

	// Выбор mesh-поверхности
	bool SetMeshSurfaceForShell();

	// --- Работа с сегментами/путём
	bool ParseElementToSegments(const API_Element& element, PathData& path);
	bool CreatePerpendicularLinesFromSegments(const PathData& path, double widthMM);
	bool Create3DShellFromPath(const PathData& path, double widthMM, double stepMM);

	// --- Получение 3D точек и профилей
	GS::Array<API_Coord3D> Get3DPointsAlongBaseLine(double stepMM);
	bool CreatePerpendicular3DPoints(double widthMM, double stepMM,
		GS::Array<API_Coord3D>& leftPoints,
		GS::Array<API_Coord3D>& rightPoints);

	// --- Сплайны / оболочки
	API_Guid CreateSplineFromPoints(const GS::Array<API_Coord>& points);
	API_Guid Create3DShell(const GS::Array<API_Coord3D>& points);
	bool     CreateRuledShell(const API_Guid& leftSplineGuid, const API_Guid& rightSplineGuid);

} // namespace ShellHelper

#endif // SHELLHELPER_HPP
