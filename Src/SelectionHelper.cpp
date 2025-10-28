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

        // Получить информацию о слое
        API_Attribute layerAttr = {};
        layerAttr.header.typeID = API_LayerID;
        layerAttr.header.index = elemHead.layer;
        if (ACAPI_Attribute_Get(&layerAttr) == NoError) {
            elemInfo.layerName = layerAttr.header.name;
        }

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

// ---------------- Изменить ID всех выделенных элементов ----------------
bool ChangeSelectedElementsID (const GS::UniString& baseID)
{
    if (baseID.IsEmpty()) return false;

    API_SelectionInfo selectionInfo = {};
    GS::Array<API_Neig> selNeigs;
    ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
    BMKillHandle((GSHandle*)&selectionInfo.marquee.coords);

    if (selNeigs.IsEmpty()) return false;

    // Используем Undo-группу для возможности отмены
    GSErrCode err = ACAPI_CallUndoableCommand("Change Elements ID", [&]() -> GSErrCode {
        for (UIndex i = 0; i < selNeigs.GetSize(); ++i) {
            // Создаем новый ID: baseID-01, baseID-02, etc.
            GS::UniString newID = baseID;
            if (selNeigs.GetSize() > 1) {
                newID += GS::UniString::Printf("-%02d", (int)(i + 1));
            }

            // Изменяем ID элемента с помощью правильной функции API
            if (ACAPI_Element_ChangeElementInfoString(&selNeigs[i].guid, &newID) != NoError) {
                // Если не удалось изменить ID, пропускаем элемент
                continue;
            }
        }
        return NoError;
    });

    return err == NoError;
}

} // namespace SelectionHelper
