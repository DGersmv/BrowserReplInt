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
bool CreateLayerFolder(const GS::UniString& folderPath, GS::Guid& folderGuid)
{
    if (folderPath.IsEmpty()) {
        folderGuid = GS::Guid(); // Пустой GUID для корневой папки
        return true; // Корневая папка уже существует
    }

    GS::Array<GS::UniString> pathParts = ParseFolderPath(folderPath);
    if (pathParts.IsEmpty()) {
        folderGuid = GS::Guid();
        return true;
    }

    // Создаем папки пошагово
    GS::Array<GS::UniString> currentPath;
    
    for (UIndex i = 0; i < pathParts.GetSize(); ++i) {
        currentPath.Push(pathParts[i]);
        
        // Проверяем, существует ли папка
        API_AttributeFolder existingFolder = {};
        existingFolder.typeID = API_LayerID;
        existingFolder.path = currentPath;
        GSErrCode err = ACAPI_Attribute_GetFolder(existingFolder);
        
        if (err != NoError) {
            // Папка не существует, создаем её
            API_AttributeFolder folder = {};
            folder.typeID = API_LayerID;
            folder.path = currentPath;
            
            err = ACAPI_Attribute_CreateFolder(folder);
            if (err != NoError) {
                ACAPI_WriteReport("[LayerHelper] Ошибка создания папки '%s' (код: %d)", true, 
                    GS::UniString::Printf("%s", currentPath[0].ToCStr().Get()).ToCStr().Get(), err);
                return false;
            }
            ACAPI_WriteReport("[LayerHelper] Создана папка: %s", false, 
                GS::UniString::Printf("%s", currentPath[0].ToCStr().Get()).ToCStr().Get());
            
            // Используем GUID созданной папки
            folderGuid = folder.guid;
            ACAPI_WriteReport("[LayerHelper] GUID созданной папки получен", false);
            ACAPI_WriteReport("[LayerHelper] Папка создана успешно, GUID не пустой: %s", false, 
                (folderGuid != GS::Guid()) ? "да" : "нет");
        } else {
            // Папка существует, используем её GUID
            folderGuid = existingFolder.guid;
            ACAPI_WriteReport("[LayerHelper] Папка уже существует: %s", false, 
                GS::UniString::Printf("%s", currentPath[0].ToCStr().Get()).ToCStr().Get());
        }
    }

    return true;
}

// ---------------- Найти слой по имени, вернуть его индекс (0 если не найден) ----------------
static API_AttributeIndex FindLayerByName(const GS::UniString& layerName)
{
    API_AttributeIndex foundIndex = APIInvalidAttributeIndex;

    // Получаем количество слоев
    GS::UInt32 layerCount = 0;
    if (ACAPI_Attribute_GetNum(API_LayerID, layerCount) != NoError || layerCount == 0)
        return foundIndex;

    // Перебираем все слои и ищем совпадение по имени
    for (Int32 i = 1; i <= static_cast<Int32>(layerCount); ++i) {
        API_Attribute attr = {};
        attr.header.typeID = API_LayerID;
        attr.header.index = ACAPI_CreateAttributeIndex(i);
        if (ACAPI_Attribute_Get(&attr) != NoError)
            continue;

        if (GS::UniString(attr.header.name) == layerName) {
            foundIndex = ACAPI_CreateAttributeIndex(i);
            break;
        }
    }

    return foundIndex;
}

// ---------------- Создать слой в указанной папке ---------------- 
bool CreateLayer(const GS::UniString& folderPath, const GS::UniString& layerName, API_AttributeIndex& layerIndex)
{
    // Если имя слоя совпадает с именем папки, или слой с таким именем уже существует —
    // не создаём новый слой, а только переносим существующий в указанную папку
    if (!layerName.IsEmpty() && (layerName == folderPath)) {
        ACAPI_WriteReport("[LayerHelper] Имя слоя совпадает с именем папки: '%s' — пропускаем создание", false, layerName.ToCStr().Get());
        API_AttributeIndex existingIdx = FindLayerByName(layerName);
        if (existingIdx.IsPositive()) {
            layerIndex = existingIdx;
            if (!folderPath.IsEmpty()) {
                ACAPI_WriteReport("[LayerHelper] Переносим существующий слой '%s' в папку '%s'", false, layerName.ToCStr().Get(), folderPath.ToCStr().Get());
                MoveLayerToFolder(layerIndex, folderPath);
            }
            return true;
        }
        // Если слоя с таким именем не нашли — продолжим обычное создание ниже
    } else {
        // Даже если имена не совпадают, стоит проверить существование слоя с таким именем,
        // чтобы избежать ошибки создания дубликата
        API_AttributeIndex existingIdx = FindLayerByName(layerName);
        if (existingIdx.IsPositive()) {
            ACAPI_WriteReport("[LayerHelper] Слой '%s' уже существует — используем его и переносим при необходимости", false, layerName.ToCStr().Get());
            layerIndex = existingIdx;
            if (!folderPath.IsEmpty()) {
                MoveLayerToFolder(layerIndex, folderPath);
            }
            return true;
        }
    }

    // Сначала создаем папку, если нужно
    GS::Guid folderGuid;
    if (!CreateLayerFolder(folderPath, folderGuid)) {
        return false;
    }

    // Создаем слой
    API_Attribute layer = {};
    layer.header.typeID = API_LayerID;
    strcpy(layer.header.name, layerName.ToCStr().Get());
    
    // Устанавливаем свойства слоя (только основные поля)
    layer.layer.conClassId = 1; // Класс соединения по умолчанию

    // Создаем слой в корне (папка будет назначена позже через MoveLayerToFolder)
    ACAPI_WriteReport("[LayerHelper] Создаем слой в корне", false);

    // Создаем слой
    GSErrCode err = ACAPI_Attribute_Create(&layer, nullptr);
    if (err != NoError) {
        ACAPI_WriteReport("[LayerHelper] Ошибка создания слоя: %s", true, layerName.ToCStr().Get());
        return false;
    }

    layerIndex = layer.header.index;
    
    // Перемещаем слой в папку, если папка указана
    if (!folderPath.IsEmpty()) {
        ACAPI_WriteReport("[LayerHelper] Пытаемся переместить слой в папку: %s", false, folderPath.ToCStr().Get());
        if (!MoveLayerToFolder(layerIndex, folderPath)) {
            ACAPI_WriteReport("[LayerHelper] Предупреждение: не удалось переместить слой в папку", false);
        }
    }
    
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

        // 3. Изменяем ID элементов (только если baseID не пустой)
        if (!params.baseID.IsEmpty()) {
            if (!ChangeSelectedElementsID(params.baseID)) {
                ACAPI_WriteReport("[LayerHelper] Ошибка изменения ID элементов", true);
                return APIERR_GENERAL;
            }
        } else {
            ACAPI_WriteReport("[LayerHelper] ID элементов не изменяются (baseID пустой)", false);
        }

        ACAPI_WriteReport("[LayerHelper] Операция завершена успешно", false);
        return NoError;
    });

    return err == NoError;
}

// ---------------- Переместить слой в папку ---------------- 
bool MoveLayerToFolder(API_AttributeIndex layerIndex, const GS::UniString& folderPath)
{
    if (folderPath.IsEmpty()) {
        return true; // Корневая папка
    }

    // Гарантируем существование папки и получаем её GUID
    GS::Guid folderGuid;
    if (!CreateLayerFolder(folderPath, folderGuid)) {
        ACAPI_WriteReport("[LayerHelper] Не удалось подготовить папку для слоя: %s", true, folderPath.ToCStr().Get());
        return false;
    }

    // Если GUID пустой — это корень, перемещать не нужно
    if (folderGuid == GS::Guid()) {
        return true;
    }

    // Пытаемся переместить слой в папку через изменение атрибута
    API_Attribute layer = {};
    layer.header.typeID = API_LayerID;
    layer.header.index = layerIndex;
    
    GSErrCode err = ACAPI_Attribute_Get(&layer);
    if (err != NoError) {
        ACAPI_WriteReport("[LayerHelper] Не удалось получить информацию о слое (код: %d)", true, err);
        ACAPI_WriteReport("[LayerHelper] Слой остался в корне, но папка создана: %s", false, folderPath.ToCStr().Get());
        return true;
    }
    
    // Пытаемся переместить слой в папку через ACAPI_Attribute_Move
    // Сначала получаем GUID слоя
    API_Attribute currentLayer = {};
    currentLayer.header.typeID = API_LayerID;
    currentLayer.header.index = layerIndex;
    
    err = ACAPI_Attribute_Get(&currentLayer);
    if (err != NoError) {
        ACAPI_WriteReport("[LayerHelper] Ошибка получения слоя для перемещения (код: %d)", true, err);
        return false;
    }
    
    // Создаем массив GUID атрибутов для перемещения (слой)
    GS::Array<GS::Guid> attributesToMove;
    // Попробуем конвертировать API_Guid в GS::Guid
    GS::UniString guidStr = APIGuidToString(currentLayer.header.guid);
    GS::Guid layerGuid(guidStr);
    attributesToMove.Push(layerGuid);
    
    // Создаем пустой массив папок (мы не перемещаем папки)
    GS::Array<API_AttributeFolder> foldersToMove;
    
    // Создаем целевую папку
    API_AttributeFolder targetFolder = {};
    targetFolder.typeID = API_LayerID;
    targetFolder.guid = folderGuid;
    
    // Перемещаем слой в папку
    ACAPI_WriteReport("[LayerHelper] Вызываем ACAPI_Attribute_Move...", false);
    err = ACAPI_Attribute_Move(foldersToMove, attributesToMove, targetFolder);
    ACAPI_WriteReport("[LayerHelper] ACAPI_Attribute_Move вернул код: %d", false, err);
    
    if (err != NoError) {
        ACAPI_WriteReport("[LayerHelper] Ошибка перемещения слоя в папку '%s' (код: %d, hex: 0x%X)", true, 
            folderPath.ToCStr().Get(), err, (unsigned int)err);
        ACAPI_WriteReport("[LayerHelper] Слой остался в корне, но папка создана: %s", false, folderPath.ToCStr().Get());
    } else {
        ACAPI_WriteReport("[LayerHelper] Слой успешно перемещен в папку: %s", false, folderPath.ToCStr().Get());
    }
    
    return true;
}




} // namespace LayerHelper
