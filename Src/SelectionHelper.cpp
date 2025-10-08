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

} // namespace SelectionHelper
