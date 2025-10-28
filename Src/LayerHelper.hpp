#ifndef LAYERHELPER_HPP
#define LAYERHELPER_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "GSRoot.hpp"

namespace LayerHelper {

    // Структура для параметров создания слоя
    struct LayerCreationParams {
        GS::UniString folderPath;  // Путь к папке (например: "Ландшафт/Растения")
        GS::UniString layerName;   // Название слоя
        GS::UniString baseID;      // Базовое название для ID элементов
    };

    // Создать папку для слоев
    bool CreateLayerFolder(const GS::UniString& folderPath, GS::Guid& folderGuid);

    // Создать слой в указанной папке
    bool CreateLayer(const GS::UniString& folderPath, const GS::UniString& layerName, API_AttributeIndex& layerIndex);

    // Переместить выделенные элементы в указанный слой
    bool MoveSelectedElementsToLayer(API_AttributeIndex layerIndex);

    // Изменить ID всех выделенных элементов
    bool ChangeSelectedElementsID(const GS::UniString& baseID);

    // Основная функция: создать папку, слой и переместить элементы
    bool CreateLayerAndMoveElements(const LayerCreationParams& params);

    // Вспомогательная функция: разбить путь к папке на массив
    GS::Array<GS::UniString> ParseFolderPath(const GS::UniString& folderPath);

    // Переместить слой в папку
    bool MoveLayerToFolder(API_AttributeIndex layerIndex, const GS::UniString& folderPath);

} // namespace LayerHelper

#endif // LAYERHELPER_HPP

