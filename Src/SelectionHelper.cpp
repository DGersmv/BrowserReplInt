#include "SelectionHelper.hpp"

namespace SelectionHelper {

// ---------------- Получить список выделенных элементов ----------------
GS::Array<ElementInfo> GetSelectedElements ()
{
    API_SelectionInfo selectionInfo = {};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selectionInfo.marquee.coords);

    GS::Array<ElementInfo> selectedElements;

    for (const API_Neig& neig : selNeigs) {
        API_Elem_Head elemHead = {};
        elemHead.guid = neig.guid;
        if (ACAPI_Element_GetHeader(&elemHead) != NoError)
            continue;

        ElementInfo elemInfo;
        elemInfo.guidStr = APIGuidToString(elemHead.guid);

        GS::UniString typeName;
        if (ACAPI_Element_GetElemTypeName(elemHead.type, typeName) == NoError)
            elemInfo.typeName = typeName;

        GS::UniString elemID;
        if (ACAPI_Element_GetElementInfoString(&elemHead.guid, &elemID) == NoError)
            elemInfo.elemID = elemID;

        selectedElements.Push(elemInfo);
    }

    return selectedElements;
}

// ---------------- Изменить выделение ----------------
void ModifySelection (const GS::UniString& elemGuidStr, SelectionModification modification)
{
    API_Guid guid = APIGuidFromString(elemGuidStr.ToCStr().Get());
    if (guid == APINULLGuid)
        return;

    API_Neig neig(guid);
    if (modification == AddToSelection) {
        ACAPI_Selection_Select({ neig }, true);   // добавить
    } else {
        ACAPI_Selection_Select({ neig }, false);  // убрать
    }
}

// ---------------- Изменить ID элемента ----------------
bool ChangeElementID (const GS::UniString& elemGuidStr, const GS::UniString& newID)
{
    API_Guid guid = APIGuidFromString(elemGuidStr.ToCStr().Get());
    if (guid == APINULLGuid) {
        ACAPI_WriteReport("ChangeElementID: Invalid GUID: " + elemGuidStr, false);
        return false;
    }

    ACAPI_WriteReport("ChangeElementID: Changing ID for GUID: " + elemGuidStr + " to: " + newID, false);

    // Попробуем использовать ACAPI_Element_ChangeElementInfoString
    GSErrCode err = ACAPI_Element_ChangeElementInfoString(&guid, &newID);
    
    if (err == NoError) {
        ACAPI_WriteReport("ChangeElementID: Success for GUID: " + elemGuidStr, false);
        return true;
    } else {
        ACAPI_WriteReport("ChangeElementID: Error " + GS::UniString::Printf("%d", err) + " for GUID: " + elemGuidStr, false);
        
        // Если функция не существует, попробуем альтернативный подход
        ACAPI_WriteReport("ChangeElementID: Trying alternative approach...", false);
        
        // Получаем элемент
        API_Element element = {};
        element.header.guid = guid;
        GSErrCode getErr = ACAPI_Element_Get(&element);
        if (getErr != NoError) {
            ACAPI_WriteReport("ChangeElementID: Failed to get element, error: " + GS::UniString::Printf("%d", getErr), false);
            return false;
        }
        
        // Создаем маску для изменения
        API_Element mask;
        ACAPI_ELEMENT_MASK_CLEAR(mask);
        
        // Попробуем изменить имя элемента в зависимости от типа
        switch (element.header.type.typeID) {
            case API_ObjectID:
                // Для объектов попробуем изменить имя
                CHCopyC(newID.ToCStr().Get(), element.object.name);
                ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, name);
                break;
            case API_WallID:
                // Для стен попробуем изменить имя
                CHCopyC(newID.ToCStr().Get(), element.wall.name);
                ACAPI_ELEMENT_MASK_SET(mask, API_WallType, name);
                break;
            case API_ColumnID:
                // Для колонн попробуем изменить имя
                CHCopyC(newID.ToCStr().Get(), element.column.name);
                ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, name);
                break;
            case API_SlabID:
                // Для плит попробуем изменить имя
                CHCopyC(newID.ToCStr().Get(), element.slab.name);
                ACAPI_ELEMENT_MASK_SET(mask, API_SlabType, name);
                break;
            default:
                ACAPI_WriteReport("ChangeElementID: Element type not supported for alternative approach", false);
                return false;
        }
        
        // Сохраняем изменения
        GSErrCode changeErr = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
        if (changeErr == NoError) {
            ACAPI_WriteReport("ChangeElementID: Success with alternative approach for GUID: " + elemGuidStr, false);
            return true;
        } else {
            ACAPI_WriteReport("ChangeElementID: Alternative approach failed, error: " + GS::UniString::Printf("%d", changeErr), false);
            return false;
        }
    }
}

// ---------------- Изменить ID всех выбранных элементов с порядковыми номерами ----------------
bool ChangeSelectedElementsID (const GS::UniString& baseID)
{
    ACAPI_WriteReport("ChangeSelectedElementsID: Starting with baseID: " + baseID, false);
    
    // Получаем список выбранных элементов
    GS::Array<ElementInfo> selectedElements = GetSelectedElements();
    if (selectedElements.IsEmpty()) {
        ACAPI_WriteReport("ChangeSelectedElementsID: No selected elements", false);
        return false;
    }

    ACAPI_WriteReport("ChangeSelectedElementsID: Found " + GS::UniString::Printf("%d", selectedElements.GetSize()) + " selected elements", false);

    bool allSuccess = true;
    int counter = 1;

    // Изменяем ID каждого элемента с порядковым номером
    for (const ElementInfo& elemInfo : selectedElements) {
        GS::UniString newID = baseID + "-" + GS::UniString::Printf("%02d", counter);
        
        ACAPI_WriteReport("ChangeSelectedElementsID: Processing element " + GS::UniString::Printf("%d", counter) + " - Type: " + elemInfo.typeName + " - Current ID: " + elemInfo.elemID + " - New ID: " + newID, false);
        
        bool success = ChangeElementID(elemInfo.guidStr, newID);
        if (!success) {
            ACAPI_WriteReport("ChangeSelectedElementsID: Failed to change ID for element " + GS::UniString::Printf("%d", counter), false);
            allSuccess = false;
        } else {
            ACAPI_WriteReport("ChangeSelectedElementsID: Successfully changed ID for element " + GS::UniString::Printf("%d", counter), false);
        }
        
        counter++;
    }

    ACAPI_WriteReport("ChangeSelectedElementsID: Completed. All success: " + GS::UniString(allSuccess ? "true" : "false"), false);
    return allSuccess;
}

} // namespace SelectionHelper
