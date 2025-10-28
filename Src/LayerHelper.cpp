#include "LayerHelper.hpp"
#include "APICommon.h"

namespace LayerHelper {

// ---------------- Разбить путь к папке на массив ----------------
GS::Array<GS::UniString> ParseFolderPath(const GS::UniString& folderPath)
{
    GS::Array<GS::UniString> pathParts;
    
    if (folderPath.IsEmpty()) {
        return pathParts;
    }

    // Разбиваем строку по символу "/"
    GS::UniString currentPath = folderPath;
    GS::UniString separator = "/";
    
    while (!currentPath.IsEmpty()) {
        Int32 separatorPos = currentPath.FindFirst(separator);
        if (separatorPos == -1) {
            // Последняя часть пути
            if (!currentPath.IsEmpty()) {
                pathParts.Push(currentPath);
            }
            break;
        } else {
            // Добавляем часть до разделителя
            GS::UniString part = currentPath.GetSubstring(0, separatorPos);
            if (!part.IsEmpty()) {
                pathParts.Push(part);
            }
            // Убираем обработанную часть и разделитель
            currentPath = currentPath.GetSubstring(separatorPos + 1, currentPath.GetLength() - separatorPos - 1);
        }
    }

    return pathParts;
}

// ---------------- Создать папку для слоев ----------------
bool CreateLayerFolder(const GS::UniString& folderPath)
{
    if (folderPath.IsEmpty()) {
        return true; // Корневая папка уже существует
    }

    GS::Array<GS::UniString> pathParts = ParseFolderPath(folderPath);
    if (pathParts.IsEmpty()) {
        return true;
    }

    // Создаем папки пошагово
    GS::Array<GS::UniString> currentPath;
    
    for (UIndex i = 0; i < pathParts.GetSize(); ++i) {
        currentPath.Push(pathParts[i]);
        
        // Создаем папку для слоев
        API_AttributeFolder folder = {};
        folder.typeID = API_LayerID;
        folder.path = currentPath;
        
        // Проверяем, существует ли папка
        API_AttributeFolder existingFolder = {};
        existingFolder.typeID = API_LayerID;
        existingFolder.path = currentPath;
        GSErrCode err = ACAPI_Attribute_GetFolder(existingFolder);
        
        if (err != NoError) {
            // Папка не существует, создаем её
            err = ACAPI_Attribute_CreateFolder(folder);
            if (err != NoError) {
                ACAPI_WriteReport("[LayerHelper] Ошибка создания папки: %s", true, folderPath.ToCStr().Get());
                return false;
            }
            ACAPI_WriteReport("[LayerHelper] Создана папка: %s", false, folderPath.ToCStr().Get());
        }
    }

    return true;
}

// ---------------- Создать слой в указанной папке ----------------
bool CreateLayer(const GS::UniString& folderPath, const GS::UniString& layerName, API_AttributeIndex& layerIndex)
{
    // Сначала создаем папку, если нужно
    if (!CreateLayerFolder(folderPath)) {
        return false;
    }

    // Создаем слой
    API_Attribute layer = {};
    layer.header.typeID = API_LayerID;
    strcpy(layer.header.name, layerName.ToCStr().Get());
    
    // Устанавливаем свойства слоя (только основные поля)
    layer.layer.conClassId = 1; // Класс соединения по умолчанию

    // Примечание: папки для слоев создаются отдельно через ACAPI_Attribute_CreateFolder
    // Слой будет создан в корне, а папка уже создана выше

    GSErrCode err = ACAPI_Attribute_Create(&layer, nullptr);
    if (err != NoError) {
        ACAPI_WriteReport("[LayerHelper] Ошибка создания слоя: %s", true, layerName.ToCStr().Get());
        return false;
    }

    layerIndex = layer.header.index;
    ACAPI_WriteReport("[LayerHelper] Создан слой: %s в папке: %s", false, layerName.ToCStr().Get(), folderPath.ToCStr().Get());
    return true;
}

// ---------------- Переместить выделенные элементы в указанный слой ----------------
bool MoveSelectedElementsToLayer(API_AttributeIndex layerIndex)
{
    // Получаем выделенные элементы
    API_SelectionInfo selectionInfo = {};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selectionInfo.marquee.coords);

    if (selNeigs.IsEmpty()) {
        ACAPI_WriteReport("[LayerHelper] Нет выделенных элементов", false);
        return false;
    }

    ACAPI_WriteReport("[LayerHelper] Перемещаем %d элементов в слой %s", false, (int)selNeigs.GetSize(), layerIndex.ToUniString().ToCStr().Get());

    // Перемещаем каждый элемент
    for (const API_Neig& neig : selNeigs) {
        API_Element element = {};
        element.header.guid = neig.guid;
        
        GSErrCode err = ACAPI_Element_Get(&element);
        if (err != NoError) {
            ACAPI_WriteReport("[LayerHelper] Ошибка получения элемента: %s", true, APIGuidToString(neig.guid).ToCStr().Get());
            continue;
        }

        // Изменяем слой элемента
        API_Element mask = {};
        ACAPI_ELEMENT_MASK_CLEAR(mask);
        
        element.header.layer = layerIndex;
        ACAPI_ELEMENT_MASK_SET(mask, API_Elem_Head, layer);

        err = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
        if (err != NoError) {
            ACAPI_WriteReport("[LayerHelper] Ошибка изменения слоя элемента: %s", true, APIGuidToString(neig.guid).ToCStr().Get());
        } else {
            ACAPI_WriteReport("[LayerHelper] Элемент перемещен в слой: %s", false, APIGuidToString(neig.guid).ToCStr().Get());
        }
    }

    return true;
}

// ---------------- Изменить ID всех выделенных элементов ----------------
bool ChangeSelectedElementsID(const GS::UniString& baseID)
{
    if (baseID.IsEmpty()) return false;

    API_SelectionInfo selectionInfo = {};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selectionInfo.marquee.coords);

    if (selNeigs.IsEmpty()) return false;

    ACAPI_WriteReport("[LayerHelper] Изменяем ID %d элементов с базовым названием: %s", false, (int)selNeigs.GetSize(), baseID.ToCStr().Get());

    // Используем Undo-группу для возможности отмены
    GSErrCode err = ACAPI_CallUndoableCommand("Change Elements ID", [&]() -> GSErrCode {
        for (UIndex i = 0; i < selNeigs.GetSize(); ++i) {
            // Создаем новый ID: baseID-01, baseID-02, etc.
            GS::UniString newID = baseID;
            if (selNeigs.GetSize() > 1) {
                newID += GS::UniString::Printf("-%02d", (int)(i + 1));
            }

            // Изменяем ID элемента
            if (ACAPI_Element_ChangeElementInfoString(&selNeigs[i].guid, &newID) != NoError) {
                ACAPI_WriteReport("[LayerHelper] Ошибка изменения ID элемента: %s", true, APIGuidToString(selNeigs[i].guid).ToCStr().Get());
                continue;
            } else {
                ACAPI_WriteReport("[LayerHelper] ID изменен: %s", false, newID.ToCStr().Get());
            }
        }
        return NoError;
    });

    return err == NoError;
}

// ---------------- Основная функция: создать папку, слой и переместить элементы ----------------
bool CreateLayerAndMoveElements(const LayerCreationParams& params)
{
    ACAPI_WriteReport("[LayerHelper] Начинаем создание папки, слоя и перемещение элементов", false);
    ACAPI_WriteReport("[LayerHelper] Папка: %s, Слой: %s, ID: %s", false, 
        params.folderPath.ToCStr().Get(), 
        params.layerName.ToCStr().Get(), 
        params.baseID.ToCStr().Get());

    // Используем Undo-группу для возможности отмены всей операции
    GSErrCode err = ACAPI_CallUndoableCommand("Create Layer and Move Elements", [&]() -> GSErrCode {
        // 1. Создаем слой
        API_AttributeIndex layerIndex;
        if (!CreateLayer(params.folderPath, params.layerName, layerIndex)) {
            ACAPI_WriteReport("[LayerHelper] Ошибка создания слоя", true);
            return APIERR_GENERAL;
        }

        // 2. Перемещаем элементы в новый слой
        if (!MoveSelectedElementsToLayer(layerIndex)) {
            ACAPI_WriteReport("[LayerHelper] Ошибка перемещения элементов", true);
            return APIERR_GENERAL;
        }

        // 3. Изменяем ID элементов
        if (!ChangeSelectedElementsID(params.baseID)) {
            ACAPI_WriteReport("[LayerHelper] Ошибка изменения ID элементов", true);
            return APIERR_GENERAL;
        }

        ACAPI_WriteReport("[LayerHelper] Операция завершена успешно", false);
        return NoError;
    });

    return err == NoError;
}

} // namespace LayerHelper
