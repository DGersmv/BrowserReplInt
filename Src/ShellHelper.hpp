#ifndef SHELLHELPER_HPP
#define SHELLHELPER_HPP

#pragma once
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "GSRoot.hpp"

namespace ShellHelper {

    // Выбрать базовую линию для создания оболочки
    bool SetBaseLineForShell();

    // Создать оболочку по базовой линии
    // widthMM - ширина перпендикулярных линий в миллиметрах
    // stepMM - шаг между перпендикулярными линиями в миллиметрах
    bool CreateShellFromLine(double widthMM, double stepMM);

    // Анализ базовой линии и получение точек с направлениями
    GS::Array<API_Coord3D> AnalyzeBaseLine(const API_Guid& lineGuid, double stepMM);

    // Генерация перпендикулярных линий в плоскости XY
    GS::Array<API_Coord3D> GeneratePerpendicularLines(
        const GS::Array<API_Coord3D>& basePoints, 
        double widthMM
    );

    // Проекция точек на 3D сетку
    GS::Array<API_Coord3D> ProjectToMesh(const GS::Array<API_Coord3D>& points);

    // Создание оболочки из 3D точек
    bool CreateShellGeometry(const GS::Array<API_Coord3D>& shellPoints);

} // namespace ShellHelper

#endif // SHELLHELPER_HPP
