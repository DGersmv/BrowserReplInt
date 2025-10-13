#include "BrowserRepl.hpp"
#include "SelectionHelper.hpp"
#include "RotateHelper.hpp"
#include "LandscapeHelper.hpp"
#include "GroundHelper.hpp"
#include "BuildHelper.hpp"
#include "GDLHelper.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>   

static const GS::Guid paletteGuid("{11bd981d-f772-4a57-8709-42e18733a0cc}");
GS::Ref<BrowserRepl> BrowserRepl::instance;

// --------------------- Helpers ---------------------
static GS::UniString LoadHtmlFromResource()
{
    GS::UniString resourceData;
    GSHandle data = RSLoadResource('DATA', ACAPI_GetOwnResModule(), 100);
    GSSize handleSize = BMhGetSize(data);
    if (data != nullptr) {
        resourceData.Append(*data, handleSize);
        BMhKill(&data);
    }
    return resourceData;
}

static GS::UniString GetStringFromJavaScriptVariable(GS::Ref<JS::Base> jsVariable)
{
    GS::Ref<JS::Value> jsValue = GS::DynamicCast<JS::Value>(jsVariable);
    if (DBVERIFY(jsValue != nullptr && jsValue->GetType() == JS::Value::STRING))
        return jsValue->GetString();
    return GS::EmptyUniString;
}

template<class Type>
static GS::Ref<JS::Base> ConvertToJavaScriptVariable(const Type& cppVariable)
{
    return new JS::Value(cppVariable);
}

template<>
GS::Ref<JS::Base> ConvertToJavaScriptVariable(const SelectionHelper::ElementInfo& elemInfo)
{
    GS::Ref<JS::Array> js = new JS::Array();
    js->AddItem(ConvertToJavaScriptVariable(elemInfo.guidStr));
    js->AddItem(ConvertToJavaScriptVariable(elemInfo.typeName));
    js->AddItem(ConvertToJavaScriptVariable(elemInfo.elemID));
    return js;
}

template<class Type>
static GS::Ref<JS::Base> ConvertToJavaScriptVariable(const GS::Array<Type>& cppArray)
{
    GS::Ref<JS::Array> newArray = new JS::Array();
    for (const Type& item : cppArray) {
        newArray->AddItem(ConvertToJavaScriptVariable(item));
    }
    return newArray;
}

// -----------------------------------------------------------------------------
// Project event handler function
// -----------------------------------------------------------------------------
static GSErrCode __ACENV_CALL NotificationHandler(API_NotifyEventID notifID, Int32 /*param*/)
{
    if (notifID == APINotify_Quit)
        BrowserRepl::DestroyInstance();
    return NoError;
}

// --- Class definition: BrowserRepl ----------------------------------------
BrowserRepl::BrowserRepl() :
    DG::Palette(ACAPI_GetOwnResModule(), BrowserReplResId, ACAPI_GetOwnResModule(), paletteGuid),
    browser(GetReference(), BrowserId)
{
    ACAPI_ProjectOperation_CatchProjectEvent(APINotify_Quit, NotificationHandler);
    Attach(*this);
    BeginEventProcessing();
    InitBrowserControl();
}

BrowserRepl::~BrowserRepl()
{
    EndEventProcessing();
}

bool BrowserRepl::HasInstance() { return instance != nullptr; }

void BrowserRepl::CreateInstance()
{
    DBASSERT(!HasInstance());
    instance = new BrowserRepl();
    ACAPI_KeepInMemory(true);
}

BrowserRepl& BrowserRepl::GetInstance()
{
    DBASSERT(HasInstance());
    return *instance;
}

void BrowserRepl::DestroyInstance() { instance = nullptr; }

void BrowserRepl::Show()
{
    DG::Palette::Show();
    browser.LoadHTML(LoadHtmlFromResource());
    SetMenuItemCheckedState(true);
}

void BrowserRepl::Hide()
{
    DG::Palette::Hide();
    SetMenuItemCheckedState(false);
}

void BrowserRepl::InitBrowserControl()
{
    browser.LoadHTML(LoadHtmlFromResource());
    RegisterACAPIJavaScriptObject();
    UpdateSelectedElementsOnHTML();
}

void BrowserRepl::LogToBrowser(const GS::UniString& msg)
{
    // Конвертация UniString → UTF-8 std::string
    std::string utf8(msg.ToCStr(CC_UTF8));

    // Экранируем спецсимволы для JS
    GS::UniString jsSafe(utf8.c_str(), CC_UTF8);
    jsSafe.ReplaceAll("\\", "\\\\");
    jsSafe.ReplaceAll("\"", "\\\"");

    browser.ExecuteJS(
        "AddLog(\"" + jsSafe + "\");"
    );
}



// ------------------ JS API registration ---------------------
void BrowserRepl::RegisterACAPIJavaScriptObject()
{
    JS::Object* jsACAPI = new JS::Object("ACAPI");

    // --- Selection API ---
    jsACAPI->AddItem(new JS::Function("GetSelectedElements", [](GS::Ref<JS::Base>) {
        return ConvertToJavaScriptVariable(SelectionHelper::GetSelectedElements());
    }));

    jsACAPI->AddItem(new JS::Function("AddElementToSelection", [](GS::Ref<JS::Base> param) {
        SelectionHelper::ModifySelection(GetStringFromJavaScriptVariable(param), SelectionHelper::AddToSelection);
        return ConvertToJavaScriptVariable(true);
    }));

    jsACAPI->AddItem(new JS::Function("RemoveElementFromSelection", [](GS::Ref<JS::Base> param) {
        SelectionHelper::ModifySelection(GetStringFromJavaScriptVariable(param), SelectionHelper::RemoveFromSelection);
        return ConvertToJavaScriptVariable(true);
    }));

    // --- Rotate API ---
    jsACAPI->AddItem(new JS::Function("RotateSelected", [](GS::Ref<JS::Base> param) {
        double angle = 0.0;
        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
            if (v->GetType() == JS::Value::DOUBLE) {
                angle = v->GetDouble();
            } else if (v->GetType() == JS::Value::STRING) {
                GS::UniString s = v->GetString();
                for (UIndex i = 0; i < s.GetLength(); ++i)
                    if (s[i] == ',') s[i] = '.';
                sscanf(s.ToCStr().Get(), "%lf", &angle);
            }
        }
        return new JS::Value(RotateHelper::RotateSelected(angle));
    }));

    jsACAPI->AddItem(new JS::Function("AlignSelectedX", [](GS::Ref<JS::Base>) {
        return new JS::Value(RotateHelper::AlignSelectedX());
    }));

    jsACAPI->AddItem(new JS::Function("RandomizeSelectedAngles", [](GS::Ref<JS::Base>) {
        return new JS::Value(RotateHelper::RandomizeSelectedAngles());
    }));

    jsACAPI->AddItem(new JS::Function("OrientObjectsToPoint", [](GS::Ref<JS::Base>) {
        return new JS::Value(RotateHelper::OrientObjectsToPoint());
    }));
	
	// --- GDL Generator ---
	jsACAPI->AddItem(new JS::Function("GenerateGDLFromSelection", [](GS::Ref<JS::Base>) {
		return new JS::Value(GDLHelper::GenerateGDLFromSelection());
	}));


    // --- Landscape API (заглушки) ---
    jsACAPI->AddItem(new JS::Function("SetDistributionLine", [](GS::Ref<JS::Base>) {
        return new JS::Value(LandscapeHelper::SetDistributionLine());
        }));

    jsACAPI->AddItem(new JS::Function("SetDistributionObject", [](GS::Ref<JS::Base>) {
        return new JS::Value(LandscapeHelper::SetDistributionObject());
    }));
    jsACAPI->AddItem(new JS::Function("SetDistributionStep", [](GS::Ref<JS::Base> param) {
        double step = 0.0;
        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param))
            step = v->GetDouble();
        return new JS::Value(LandscapeHelper::SetDistributionStep(step));
    }));
    jsACAPI->AddItem(new JS::Function("SetDistributionCount", [](GS::Ref<JS::Base> param) {
        int count = 0;
        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param))
            count = (int)v->GetDouble();
        return new JS::Value(LandscapeHelper::SetDistributionCount(count));
    }));
    jsACAPI->AddItem(new JS::Function("DistributeNow", [](GS::Ref<JS::Base> param) {
        double step = 0.0;
        int    count = 0;

        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
            switch (v->GetType()) {
            case JS::Value::DOUBLE:
            case JS::Value::INTEGER: {
                // если прилетело просто число — трактуем как шаг
                step = v->GetDouble();
                break;
            }
            case JS::Value::STRING: {
                GS::UniString s = v->GetString();
                // заменим запятые на точки
                for (UIndex i = 0; i < s.GetLength(); ++i) if (s[i] == ',') s[i] = '.';
                const char* c = s.ToCStr().Get();

                if (std::strncmp(c, "step:", 5) == 0) {
                    double tmp = 0.0;
                    std::sscanf(c + 5, "%lf", &tmp);
                    step = tmp;
                }
                else if (std::strncmp(c, "count:", 6) == 0) {
                    int tmp = 0;
                    std::sscanf(c + 6, "%d", &tmp);
                    count = tmp;
                }
                else {
                    // запасной вариант — пробуем как число шага
                    double tmp = 0.0;
                    std::sscanf(c, "%lf", &tmp);
                    step = tmp;
                }
                break;
            }
            default: break;
            }
        }

        // Диагностика
        if (BrowserRepl::HasInstance()) {
            GS::UniString dbg; dbg.Printf("[JS] DistributeNow parsed: step=%.6f, count=%d", step, count);
            BrowserRepl::GetInstance().LogToBrowser(dbg);
        }

        return new JS::Value(LandscapeHelper::DistributeSelected(step, count));
        }));

    // --- Ground API ---
    jsACAPI->AddItem(new JS::Function("SetGroundSurface", [](GS::Ref<JS::Base>) {
        return new JS::Value(GroundHelper::SetGroundSurface());
        }));
    jsACAPI->AddItem(new JS::Function("SetGroundObjects", [](GS::Ref<JS::Base>) {
        return new JS::Value(GroundHelper::SetGroundObjects());
        }));
    jsACAPI->AddItem(new JS::Function("ApplyGroundOffset", [](GS::Ref<JS::Base> param) {
        double offset = 0.0;

        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
            switch (v->GetType()) {
            case JS::Value::DOUBLE:
            case JS::Value::INTEGER:
                offset = v->GetDouble();
                break;
            case JS::Value::STRING: {
                GS::UniString s = v->GetString();
                for (UIndex i = 0; i < s.GetLength(); ++i) if (s[i] == ',') s[i] = '.';
                double parsed = 0.0;
                std::sscanf(s.ToCStr().Get(), "%lf", &parsed);
                offset = parsed;
                break;
            }
            default: break;
            }
        }

        // --- Автодетект единиц (страховка):
        // если пришло что-то по масштабe похоже на миллиметры (например 1500),
        // переводим в метры.
        bool unitWasMM = false;
        if (std::fabs(offset) > 100.0) {   // порог можно подправить под твои проекты
            offset /= 1000.0;
            unitWasMM = true;
        }

        // Диагностика
        GS::UniString dbg; dbg.Printf("[JS->C++] ApplyGroundOffset parsed=%.6f %s",
            offset, unitWasMM ? "(auto mm to m)" : "(m)");
        if (BrowserRepl::HasInstance()) BrowserRepl::GetInstance().LogToBrowser(dbg);
        ACAPI_WriteReport("%s", false, dbg.ToCStr().Get());

        return new JS::Value(GroundHelper::ApplyGroundOffset(offset));
        }));

    // --- Build API (заглушки) ---
    jsACAPI->AddItem(new JS::Function("SetCurveForSlab", [](GS::Ref<JS::Base>) {
        return new JS::Value(BuildHelper::SetCurveForSlab());
    }));
    
    jsACAPI->AddItem(new JS::Function("CreateSlabAlongCurve", [](GS::Ref<JS::Base> param) {
        double width = 0.0;

        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
            if (v->GetType() == JS::Value::DOUBLE) {
                width = v->GetDouble();
            }
            else if (v->GetType() == JS::Value::STRING) {
                GS::UniString s = v->GetString();
                for (UIndex i = 0; i < s.GetLength(); ++i)
                    if (s[i] == ',') s[i] = '.';
                double parsed = 0.0;
                std::sscanf(s.ToCStr().Get(), "%lf", &parsed);
                width = parsed;
            }
        }

        return new JS::Value(BuildHelper::CreateSlabAlongCurve(width));
        }));
        
    jsACAPI->AddItem(new JS::Function("SetCurveForShell", [](GS::Ref<JS::Base>) {
        return new JS::Value(BuildHelper::SetCurveForShell());
    }));
    jsACAPI->AddItem(new JS::Function("SetMeshForShell", [](GS::Ref<JS::Base>) {
        return new JS::Value(BuildHelper::SetMeshForShell());
    }));
    jsACAPI->AddItem(new JS::Function("CreateShellAlongCurve", [](GS::Ref<JS::Base> param) {
        double width = 0.0;
        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param))
            width = v->GetDouble();
        return new JS::Value(BuildHelper::CreateShellAlongCurve(width));
    }));

    jsACAPI->AddItem(new JS::Function("LogMessage", [](GS::Ref<JS::Base> param) {
        if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
            if (v->GetType() == JS::Value::STRING) {
                GS::UniString s = v->GetString();
                BrowserRepl::GetInstance().browser.ExecuteJS(
                    "AddLog(" + GS::UniString("\"") + s + GS::UniString("\"") + ");"
                );
            }
        }
        return new JS::Value(true);
        }));

    // --- Register object in the browser ---
    browser.RegisterAsynchJSObject(jsACAPI);
}

// ------------------- Palette and Events ----------------------
void BrowserRepl::UpdateSelectedElementsOnHTML()
{
    browser.ExecuteJS("UpdateSelectedElements ()");
}

void BrowserRepl::SetMenuItemCheckedState(bool isChecked)
{
    API_MenuItemRef itemRef = {};
    GSFlags itemFlags = {};

    itemRef.menuResID = BrowserReplMenuResId;
    itemRef.itemIndex = BrowserReplMenuItemIndex;

    ACAPI_MenuItem_GetMenuItemFlags(&itemRef, &itemFlags);
    if (isChecked) itemFlags |= API_MenuItemChecked;
    else           itemFlags &= ~API_MenuItemChecked;
    ACAPI_MenuItem_SetMenuItemFlags(&itemRef, &itemFlags);
}

void BrowserRepl::PanelResized(const DG::PanelResizeEvent& ev)
{
    BeginMoveResizeItems();
    browser.Resize(ev.GetHorizontalChange(), ev.GetVerticalChange());
    EndMoveResizeItems();
}

void BrowserRepl::PanelCloseRequested(const DG::PanelCloseRequestEvent&, bool* accepted)
{
    Hide();
    *accepted = true;
}

GSErrCode __ACENV_CALL BrowserRepl::SelectionChangeHandler(const API_Neig*)
{
    if (BrowserRepl::HasInstance())
        BrowserRepl::GetInstance().UpdateSelectedElementsOnHTML();
    return NoError;
}

GSErrCode __ACENV_CALL BrowserRepl::PaletteControlCallBack(Int32, API_PaletteMessageID messageID, GS::IntPtr param)
{
    switch (messageID) {
    case APIPalMsg_OpenPalette:
        if (!HasInstance())
            CreateInstance();
        GetInstance().Show();
        break;

    case APIPalMsg_ClosePalette:
        if (!HasInstance())
            break;
        GetInstance().Hide();
        break;

    case APIPalMsg_HidePalette_Begin:
        if (HasInstance() && GetInstance().IsVisible())
            GetInstance().Hide();
        break;

    case APIPalMsg_HidePalette_End:
        if (HasInstance() && !GetInstance().IsVisible())
            GetInstance().Show();
        break;

    case APIPalMsg_DisableItems_Begin:
        if (HasInstance() && GetInstance().IsVisible())
            GetInstance().DisableItems();
        break;

    case APIPalMsg_DisableItems_End:
        if (HasInstance() && GetInstance().IsVisible())
            GetInstance().EnableItems();
        break;

    case APIPalMsg_IsPaletteVisible:
        *(reinterpret_cast<bool*> (param)) = HasInstance() && GetInstance().IsVisible();
        break;

    default:
        break;
    }
    return NoError;
}

GSErrCode BrowserRepl::RegisterPaletteControlCallBack()
{
    return ACAPI_RegisterModelessWindow(
        GS::CalculateHashValue(paletteGuid),
        PaletteControlCallBack,
        API_PalEnabled_FloorPlan + API_PalEnabled_Section + API_PalEnabled_Elevation +
        API_PalEnabled_InteriorElevation + API_PalEnabled_3D + API_PalEnabled_Detail +
        API_PalEnabled_Worksheet + API_PalEnabled_Layout + API_PalEnabled_DocumentFrom3D,
        GSGuid2APIGuid(paletteGuid));
}
