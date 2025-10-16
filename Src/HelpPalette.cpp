// ========================= HelpPalette.cpp (замени соответствующие части) =========================
#include "HelpPalette.hpp"

// Archicad API
#include "APIEnvir.h"
#include "ACAPinc.h"

// Браузерный контрол
#include "DGBrowser.hpp"

// ------------ file-local palette callback (НЕ член класса) ------------
static GSErrCode __ACENV_CALL HelpPalette_CB(Int32 /*refCon*/, API_PaletteMessageID messageID, GS::IntPtr param)
{
	switch (messageID) {
	case APIPalMsg_OpenPalette:
		if (!HelpPalette::HasInstance()) HelpPalette::CreateInstance();
		HelpPalette::GetInstance().Show();
		break;

	case APIPalMsg_ClosePalette:
		if (HelpPalette::HasInstance()) HelpPalette::GetInstance().Hide();
		break;

	case APIPalMsg_HidePalette_Begin:
		if (HelpPalette::HasInstance() && HelpPalette::GetInstance().IsVisible())
			HelpPalette::GetInstance().Hide();
		break;

	case APIPalMsg_HidePalette_End:
		if (HelpPalette::HasInstance() && !HelpPalette::GetInstance().IsVisible())
			HelpPalette::GetInstance().Show();
		break;

	case APIPalMsg_DisableItems_Begin:
		if (HelpPalette::HasInstance() && HelpPalette::GetInstance().IsVisible())
			HelpPalette::GetInstance().DisableItems();
		break;

	case APIPalMsg_DisableItems_End:
		if (HelpPalette::HasInstance() && HelpPalette::GetInstance().IsVisible())
			HelpPalette::GetInstance().EnableItems();
		break;

	case APIPalMsg_IsPaletteVisible:
		if (param != 0)
			*(reinterpret_cast<bool*> (param)) =
			HelpPalette::HasInstance() && HelpPalette::GetInstance().IsVisible();
		break;

	default:
		break;
	}
	return NoError;
}

// -------------------- static members --------------------
GS::Ref<HelpPalette> HelpPalette::s_instance(nullptr);
const GS::Guid HelpPalette::s_guid("{06a4b1bb-f5ca-400b-bab5-ce65186d5d7e}");

// -------------------- ctor / dtor --------------------
HelpPalette::HelpPalette()
	: DG::Palette(ACAPI_GetOwnResModule(), HelpPaletteResId, ACAPI_GetOwnResModule(), s_guid)
{
	m_browserCtrl = new DG::Browser(GetReference(), HelpBrowserCtrlId);
	Attach(*this);
	BeginEventProcessing();
	Init();
}

HelpPalette::~HelpPalette()
{
	EndEventProcessing();
	delete m_browserCtrl;
	m_browserCtrl = nullptr;
}

// -------------------- singleton --------------------
bool HelpPalette::HasInstance() { return s_instance != nullptr; }

void HelpPalette::CreateInstance()
{
	DBASSERT(!HasInstance());
	s_instance = new HelpPalette();
	ACAPI_KeepInMemory(true);
}

HelpPalette& HelpPalette::GetInstance()
{
	DBASSERT(HasInstance());
	return *s_instance;
}

void HelpPalette::DestroyInstance() { s_instance = nullptr; }

// -------------------- internals --------------------
void HelpPalette::Init()
{
	// if (m_browserCtrl) m_browserCtrl->LoadHTML ("<html><body>Help</body></html>");
}

void HelpPalette::SetURL(const GS::UniString& url)
{
	if (m_browserCtrl == nullptr || url.IsEmpty()) return;
	m_browserCtrl->LoadURL(url);
}

// -------------------- public API --------------------
void HelpPalette::ShowWithURL(const GS::UniString& url)
{
	if (!HasInstance())
		CreateInstance();

	GetInstance().Show();

	if (!url.IsEmpty())
		GetInstance().SetURL(url);
}

void HelpPalette::HidePalette()
{
	if (!HasInstance())
		return;

	GetInstance().Hide();
}

// -------------------- DG::PanelObserver --------------------
void HelpPalette::PanelResized(const DG::PanelResizeEvent& ev)
{
	BeginMoveResizeItems();
	if (m_browserCtrl != nullptr)
		m_browserCtrl->Resize(ev.GetHorizontalChange(), ev.GetVerticalChange());
	EndMoveResizeItems();
}

void HelpPalette::PanelCloseRequested(const DG::PanelCloseRequestEvent&, bool* accepted)
{
	HidePalette();
	if (accepted) *accepted = true;
}

// -------------------- Registration --------------------
GSErrCode HelpPalette::RegisterPaletteControlCallBack()
{
	return ACAPI_RegisterModelessWindow(
		GS::CalculateHashValue(s_guid),
		HelpPalette_CB,   // <-- свободная функция из этого .cpp
		API_PalEnabled_FloorPlan |
		API_PalEnabled_Section |
		API_PalEnabled_Elevation |
		API_PalEnabled_InteriorElevation |
		API_PalEnabled_3D |
		API_PalEnabled_Detail |
		API_PalEnabled_Worksheet |
		API_PalEnabled_Layout |
		API_PalEnabled_DocumentFrom3D,
		GSGuid2APIGuid(s_guid)
	);
}