#ifndef SHELLHELPER_HPP
#define SHELLHELPER_HPP

#pragma once
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "GSRoot.hpp"

namespace ShellHelper {

    // Глобальные переменные для хранения выбранных элементов
    extern API_Guid g_baseLineGuid;      // GUID базовой линии
    extern API_Guid g_meshSurfaceGuid;   // GUID Mesh поверхности

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

    // Создание перпендикулярных линий от базовой линии
    bool CreatePerpendicularLines(const API_Element& baseLine, double widthMM);

    // Выбрать Mesh поверхность для получения высот
    bool SetMeshSurfaceForShell();

    // Новые функции для поддержки полилиний, арок и сплайнов
    
    // Структуры для работы с сегментами (из LandscapeHelper)
    enum class SegType { Line, Arc, Cubic };
    
    struct Seg {
        SegType type = SegType::Line;
        
        // Line
        API_Coord A{}, B{};
        
        // Arc
        API_Coord C{};          // центр
        double    r = 0.0;
        double    a0 = 0.0;     // стартовый угол (рад)
        double    a1 = 0.0;     // конечный угол (рад)
        bool      ccw = true;   // направление обхода (true = CCW)
        
        // Общая длина сегмента
        double len = 0.0;
    };
    
    struct PathData {
        GS::Array<Seg> segs;
        double total = 0.0;
    };
    
    // Парсинг элемента в сегменты (поддержка Line, Polyline, Arc, Circle, Spline)
    bool ParseElementToSegments(const API_Element& element, PathData& path);
    
    // Создание перпендикулярных линий от сегментов
    bool CreatePerpendicularLinesFromSegments(const PathData& path, double widthMM);

    // Новые функции для создания 3D оболочки через Ruled Shell
    
    // Получение 3D точек вдоль базовой линии с учетом высот от Mesh
    GS::Array<API_Coord3D> Get3DPointsAlongBaseLine(double stepMM);
    
    // Создание левых и правых перпендикулярных точек с высотами от Mesh
    bool CreatePerpendicular3DPoints(double widthMM, double stepMM, 
                                    GS::Array<API_Coord3D>& leftPoints, 
                                    GS::Array<API_Coord3D>& rightPoints);
    
// Создание Spline из 2D точек
API_Guid CreateSplineFromPoints(const GS::Array<API_Coord>& points);

// Создание 3D Shell из массива точек
API_Guid Create3DShell(const GS::Array<API_Coord3D>& points);
    
    // Создание Ruled Shell между двумя Spline
    bool CreateRuledShell(const API_Guid& leftSplineGuid, const API_Guid& rightSplineGuid);
    
    // Создание 3D оболочки через Ruled Shell
    bool Create3DShellFromPath(const PathData& path, double widthMM, double stepMM);

} // namespace ShellHelper

#endif // SHELLHELPER_HPP
