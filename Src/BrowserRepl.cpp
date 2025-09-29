// *****************************************************************************
// Source code for the BrowserRepl class
// *****************************************************************************

// ---------------------------------- Includes ---------------------------------
#include "BrowserRepl.hpp"
#include "APIEnvir.h"
#include "ACAPinc.h"        // для ACAPI_ELEMENT_MASK_* макросов
#include <random>
#include <cmath>
#include <cstdlib>
#include <cstdio>   // для sscanf

// --------------------- Math helpers ---------------------
constexpr double PI = 3.14159265358979323846;
constexpr double DegToRad(double deg) { return deg * PI / 180.0; }

// --------------------- Globals ---------------------
static const GS::Guid paletteGuid("{11bd981d-f772-4a57-8709-42e18733a0cc}");
GS::Ref<BrowserRepl> BrowserRepl::instance;
static GS::UniString g_rotatePivotMode = "origin";   // "origin" | "bbox"

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
GS::Ref<JS::Base> ConvertToJavaScriptVariable(const BrowserRepl::ElementInfo& elemInfo)
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
	switch (notifID) {
	case APINotify_Quit:
		BrowserRepl::DestroyInstance();
		break;
	}
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
	browser.LoadHTML(LoadHtmlFromResource());   // вернуть «домашнюю» HTML
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

// ------------------ JS API registration ---------------------
void BrowserRepl::RegisterACAPIJavaScriptObject()
{
	JS::Object* jsACAPI = new JS::Object("ACAPI");

	// --- Selection API ---
	jsACAPI->AddItem(new JS::Function("GetSelectedElements", [](GS::Ref<JS::Base>) {
		ACAPI_WriteReport("[JS] GetSelectedElements", false);
		return ConvertToJavaScriptVariable(GetSelectedElements());
		}));

	jsACAPI->AddItem(new JS::Function("AddElementToSelection", [](GS::Ref<JS::Base> param) {
		ModifySelection(GetStringFromJavaScriptVariable(param), AddToSelection);
		return ConvertToJavaScriptVariable(true);
		}));

	jsACAPI->AddItem(new JS::Function("RemoveElementFromSelection", [](GS::Ref<JS::Base> param) {
		ModifySelection(GetStringFromJavaScriptVariable(param), RemoveFromSelection);
		return ConvertToJavaScriptVariable(true);
		}));

	// --- Set pivot mode for rotation (origin | bbox) ---
	jsACAPI->AddItem(new JS::Function("SetRotateMode", [](GS::Ref<JS::Base> param) {
		GS::UniString m = GetStringFromJavaScriptVariable(param);
		g_rotatePivotMode = m.IsEqual("bbox") ? GS::UniString("bbox") : GS::UniString("origin");

		GS::UniString msg;
		msg.Append("[JS] SetRotateMode = ");
		msg.Append(g_rotatePivotMode);
		ACAPI_WriteReport(msg.ToCStr().Get(), false);

		return new JS::Value(true);
		}));

	// --- Randomize angles for selected (Object/Lamp/Column) ---
	jsACAPI->AddItem(new JS::Function("RandomizeSelectedAngles", [](GS::Ref<JS::Base>) {
		ACAPI_WriteReport("[JS] RandomizeSelectedAngles called", false);

		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) {
			ACAPI_WriteReport("[Random] selection empty -> skip", false);
			return new JS::Value(false);
		}

		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_real_distribution<double> dist(0.0, 2.0 * PI);

		GSErrCode res = ACAPI_CallUndoableCommand("Randomize angles", [&]() -> GSErrCode {
			for (const API_Neig& neig : selNeigs) {
				API_Element element = {};
				element.header.guid = neig.guid;

				if (ACAPI_Element_Get(&element) != NoError)
					continue;

				const double rnd = dist(gen);

				API_Element mask; ACAPI_ELEMENT_MASK_CLEAR(mask);
				bool apply = false;

				switch (element.header.type.typeID) {
				case API_ObjectID:
					element.object.angle = rnd;
					ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, angle);
					apply = true;
					break;

				case API_LampID:
					element.lamp.angle = rnd;
					ACAPI_ELEMENT_MASK_SET(mask, API_LampType, angle);   // ВАЖНО: лампа -> API_LampType
					apply = true;
					break;

				case API_ColumnID:
					element.column.axisRotationAngle = rnd;
					ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, axisRotationAngle);
					apply = true;
					break;

				default:
					break;
				}

				if (apply) {
					(void)ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
				}
			}
			return NoError;
			});

		ACAPI_WriteReport("[Random] done", res != NoError);
		return new JS::Value(res == NoError);
		}));


	// --- Rotate selection by entered angle (supports origin/bbox) ---
	// HTML: ACAPI.SetRotateMode(mode); ACAPI.RotateSelected(angleString);
	// --- Rotate selection (columns / objects / lamps) ---
	jsACAPI->AddItem(new JS::Function("RotateSelected", [](GS::Ref<JS::Base> param) {
		ACAPI_WriteReport("[JS] RotateSelected called", false);

		
		double angleDeg = 0.0;
		int jsType = -1;

		if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
			jsType = (int)v->GetType();

			if (v->GetType() == JS::Value::DOUBLE) {
				angleDeg = v->GetDouble();

			}
			else if (v->GetType() == JS::Value::STRING) {
				GS::UniString s = v->GetString();

				
				for (UIndex i = 0; i < s.GetLength(); ++i) {
					if (s[i] == ',') s[i] = '.';
				}

				double tmp = 0.0;
				if (sscanf(s.ToCStr().Get(), "%lf", &tmp) == 1) {
					angleDeg = tmp;
				}
			}
			else {
				
				GS::UniString s;
				try { s = v->GetString(); }
				catch (...) {}
				if (!s.IsEmpty()) {
					for (UIndex i = 0; i < s.GetLength(); ++i) if (s[i] == ',') s[i] = '.';
					double tmp = 0.0;
					if (sscanf(s.ToCStr().Get(), "%lf", &tmp) == 1) angleDeg = tmp;
				}
			}
		}

		GS::UniString msg;
		msg.Printf("[Rotate] parsed angleDeg=%.6f (jsType=%d)", angleDeg, jsType);
		ACAPI_WriteReport(msg.ToCStr().Get(), false);

		if (fabs(angleDeg) < 1e-6) {
			ACAPI_WriteReport("[Rotate] angle ~ 0, skip", false);
			return new JS::Value(false);
		}


		
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) {
			ACAPI_WriteReport("[Rotate] selection empty", false);
			return new JS::Value(false);
		}

		const double addRad = DegToRad(angleDeg);
		GS::UInt32 changedByParams = 0;

		
		GSErrCode cmdErr = ACAPI_CallUndoableCommand("Rotate Selected (Columns/Objects/Lamps)", [&]() -> GSErrCode {
			for (const API_Neig& n : selNeigs) {
				API_Element element = {};
				element.header.guid = n.guid;

				GSErrCode gerr = ACAPI_Element_Get(&element);
				if (gerr != NoError) {
					GS::UniString msg; msg.Printf("[Rotate] Get failed guid=%T err=%d", APIGuidToString(n.guid).ToPrintf(), gerr);
					ACAPI_WriteReport(msg.ToCStr().Get(), true);
					continue;
				}

				API_Element mask; ACAPI_ELEMENT_MASK_CLEAR(mask);
				bool canParamRotate = false;

				switch (element.header.type.typeID) {
				case API_ColumnID:
					element.column.axisRotationAngle += addRad;
					ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, axisRotationAngle);
					canParamRotate = true;
					break;

				case API_ObjectID:
					element.object.angle += addRad;
					ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, angle);
					canParamRotate = true;
					break;

				case API_LampID:
					element.lamp.angle += addRad;
					
					ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, angle);
					canParamRotate = true;
					break;

				default:
					break;
				}

				if (!canParamRotate)
					continue;

				gerr = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);

				GS::UniString msg;
				msg.Printf("[Rotate:param] guid=%T err=%d", APIGuidToString(n.guid).ToPrintf(), gerr);
				ACAPI_WriteReport(msg.ToCStr().Get(), gerr != NoError);

				if (gerr == NoError)
					++changedByParams;
			}
			return NoError;
			});

		
		if (changedByParams == 0) {
			ACAPI_WriteReport("[Rotate] param-change did not modify anything, fallback to ACAPI_Element_Edit", false);

			API_Coord3D begC = { 1.0, 0.0, 0.0 };
			API_Coord3D endC = { cos(addRad), sin(addRad), 0.0 };
			API_Coord    orig = { 0.0, 0.0 };

			API_EditPars ep = {};
			ep.typeID = APIEdit_Rotate;
			ep.begC = begC;
			ep.endC = endC;
			ep.origC.x = orig.x;
			ep.origC.y = orig.y;
			ep.withDelete = true;

			GSErrCode eerr = ACAPI_Element_Edit(&selNeigs, ep);

			GS::UniString msg; msg.Printf("[Rotate:edit] items=%d err=%d", selNeigs.GetSize(), eerr);
			ACAPI_WriteReport(msg.ToCStr().Get(), eerr != NoError);
			cmdErr = eerr;
		}

		ACAPI_WriteReport("[Rotate] done", cmdErr != NoError);
		return new JS::Value(cmdErr == NoError);
		}));




	// --- Align selection to X axis (angle = 0) ---
	jsACAPI->AddItem(new JS::Function("AlignSelectedX", [](GS::Ref<JS::Base>) {
		ACAPI_WriteReport("[JS] AlignSelectedX called", false);

		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) {
			ACAPI_WriteReport("[AlignX] selection empty", false);
			return new JS::Value(false);
		}

		GSErrCode cmdErr = ACAPI_CallUndoableCommand("Align to X (Columns/Objects/Lamps)", [&]() -> GSErrCode {
			for (const API_Neig& n : selNeigs) {
				API_Element element = {};
				element.header.guid = n.guid;

				GSErrCode gerr = ACAPI_Element_Get(&element);
				if (gerr != NoError) {
					GS::UniString msg; msg.Printf("[AlignX] Get failed guid=%T err=%d", APIGuidToString(n.guid).ToPrintf(), gerr);
					ACAPI_WriteReport(msg.ToCStr().Get(), true);
					continue;
				}

				API_Element mask; ACAPI_ELEMENT_MASK_CLEAR(mask);

				switch (element.header.type.typeID) {
				case API_ColumnID:
					element.column.axisRotationAngle = 0.0;
					ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, axisRotationAngle);
					break;

				case API_ObjectID:
					element.object.angle = 0.0;
					ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, angle);
					break;

				case API_LampID:
					element.lamp.angle = 0.0;
					ACAPI_ELEMENT_MASK_SET(mask, API_LampType, angle);
					break;

				default:
					continue;
				}

				gerr = ACAPI_Element_Change(&element, &mask, nullptr, 0, true);

				GS::UniString msg;
				msg.Printf("[AlignX] changed guid=%T err=%d", APIGuidToString(n.guid).ToPrintf(), gerr);
				ACAPI_WriteReport(msg.ToCStr().Get(), gerr != NoError);
			}
			return NoError;
			});

		ACAPI_WriteReport("[AlignX] done", cmdErr != NoError);
		return new JS::Value(cmdErr == NoError);
		}));


	// --- Orient objects to a picked point ---
	jsACAPI->AddItem(new JS::Function("OrientObjectsToPoint", [](GS::Ref<JS::Base> param) {
		ACAPI_WriteReport("[JS] OrientObjectsToPoint called", false);

		// 1) режим (origin | bbox)
		GS::UniString mode = "origin";
		if (GS::Ref<JS::Value> v = GS::DynamicCast<JS::Value>(param)) {
			if (v->GetType() == JS::Value::STRING)
				mode = v->GetString();
		}

		// лог
		{
			GS::UniString log; log.Append("[Orient] mode = "); log.Append(mode);
			ACAPI_WriteReport(log.ToCStr().Get(), false);
		}

		// 2) запрос точки
		API_GetPointType pt = {};
		CHTruncate("Укажите точку для ориентации объектов", pt.prompt, sizeof(pt.prompt));
		if (ACAPI_UserInput_GetPoint(&pt) != NoError) {
			ACAPI_WriteReport("[Orient] Point picking cancelled or failed", true);
			return new JS::Value(false);
		}
		const API_Coord target = { pt.pos.x, pt.pos.y };

		// 3) выделение
		API_SelectionInfo selInfo = {};
		GS::Array<API_Neig> selNeigs;
		ACAPI_Selection_Get(&selInfo, &selNeigs, false, false);
		BMKillHandle((GSHandle*)&selInfo.marquee.coords);

		if (selNeigs.IsEmpty()) {
			ACAPI_WriteReport("[Orient] selection empty", false);
			return new JS::Value(false);
		}

		// 4) логика
		GSErrCode err = ACAPI_CallUndoableCommand("Orient Objects to Point", [&]() -> GSErrCode {
			for (const API_Neig& n : selNeigs) {
				API_Element element = {};
				element.header.guid = n.guid;

				if (ACAPI_Element_Get(&element) != NoError)
					continue;

				API_Coord objPos = {};
				bool canOrient = true;

				if (mode.IsEqual("origin")) {
					switch (element.header.type.typeID) {
					case API_ObjectID: objPos = element.object.pos; break;
					case API_LampID:   objPos = element.lamp.pos;   break;
					case API_ColumnID: objPos.x = element.column.origoPos.x; objPos.y = element.column.origoPos.y; break;
					default: canOrient = false; break;
					}
				}
				else if (mode.IsEqual("bbox")) {
					API_Box3D bounds;
					if (ACAPI_Element_CalcBounds(&element.header, &bounds) == NoError) {
						objPos.x = (bounds.xMin + bounds.xMax) * 0.5;
						objPos.y = (bounds.yMin + bounds.yMax) * 0.5;
					}
					else {
						canOrient = false;
					}
				}

				if (!canOrient) continue;

				const double dx = target.x - objPos.x;
				const double dy = target.y - objPos.y;
				const double newAngle = std::atan2(dy, dx);

				API_Element mask; ACAPI_ELEMENT_MASK_CLEAR(mask);

				switch (element.header.type.typeID) {
				case API_ObjectID:
					element.object.angle = newAngle;
					ACAPI_ELEMENT_MASK_SET(mask, API_ObjectType, angle);
					break;
				case API_LampID:
					element.lamp.angle = newAngle;
					ACAPI_ELEMENT_MASK_SET(mask, API_LampType, angle); // лампа -> API_LampType
					break;
				case API_ColumnID:
					element.column.axisRotationAngle = newAngle;
					ACAPI_ELEMENT_MASK_SET(mask, API_ColumnType, axisRotationAngle);
					break;
				default:
					break;
				}

				(void)ACAPI_Element_Change(&element, &mask, nullptr, 0, true);
			}
			return NoError;
			});

		ACAPI_WriteReport("[Orient] done", err != NoError);
		return new JS::Value(err == NoError);
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
	API_MenuItemRef	itemRef = {};
	GSFlags			itemFlags = {};

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

GS::Array<BrowserRepl::ElementInfo> BrowserRepl::GetSelectedElements()
{
	API_SelectionInfo	selectionInfo = {};
	GS::Array<API_Neig>	selNeigs;
	ACAPI_Selection_Get(&selectionInfo, &selNeigs, false, false);
	BMKillHandle((GSHandle*)&selectionInfo.marquee.coords);

	GS::Array<BrowserRepl::ElementInfo> selectedElements;
	for (const API_Neig& neig : selNeigs) {
		API_Elem_Head elemHead = {};
		elemHead.guid = neig.guid;
		ACAPI_Element_GetHeader(&elemHead);

		ElementInfo elemInfo;
		elemInfo.guidStr = APIGuidToString(elemHead.guid);
		ACAPI_Element_GetElemTypeName(elemHead.type, elemInfo.typeName);
		ACAPI_Element_GetElementInfoString(&elemHead.guid, &elemInfo.elemID);
		selectedElements.Push(elemInfo);
	}
	return selectedElements;
}

void BrowserRepl::ModifySelection(const GS::UniString& elemGuidStr, BrowserRepl::SelectionModification modification)
{
	ACAPI_Selection_Select({ API_Neig(APIGuidFromString(elemGuidStr.ToCStr().Get())) }, modification == AddToSelection);
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
