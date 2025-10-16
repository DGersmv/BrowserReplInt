// ========================= HelpPalette.hpp (замени целиком) =========================
#pragma once

#include "GSRoot.hpp"
#include "GSGuid.hpp"
#include "UniString.hpp"
#include "DGModule.hpp"
#include "ResourceIds.hpp"

// forward-declare; полный заголовок подключим в .cpp
namespace DG { class Browser; }

class HelpPalette : public DG::Palette, public DG::PanelObserver
{
public:
	// ---- Singleton ----
	static bool         HasInstance();
	static void         CreateInstance();
	static HelpPalette& GetInstance();
	static void         DestroyInstance();

	// ---- API ----
	static void         ShowWithURL(const GS::UniString& url);
	static void         HidePalette();
	static GSErrCode    RegisterPaletteControlCallBack();

	// Деструктор обязан быть public для GS::Ref<>
	virtual ~HelpPalette();

private:
	HelpPalette();
	void        Init();
	void        SetURL(const GS::UniString& url);

	// DG::PanelObserver
	void PanelResized(const DG::PanelResizeEvent& ev) override;
	void PanelCloseRequested(const DG::PanelCloseRequestEvent& ev, bool* accepted) override;

private:
	static GS::Ref<HelpPalette> s_instance;
	static const GS::Guid       s_guid;

	DG::Browser* m_browserCtrl = nullptr;
};