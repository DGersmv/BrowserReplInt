// *****************************************************************************
// Header file for BrowserPalette class
// *****************************************************************************

#ifndef BROWSERREPL_HPP
#define BROWSERREPL_HPP

// ---------------------------------- Includes ---------------------------------

#include "APIEnvir.h"
#include "ACAPinc.h"		// also includes APIdefs.h
#include "DGModule.hpp"
#include "DGBrowser.hpp"

#define BrowserReplResId 32500
#define BrowserReplMenuResId 32500
#define BrowserReplMenuItemIndex 1

// --- Class declaration: BrowserPalette ------------------------------------------

class BrowserRepl final : public DG::Palette,
							 public DG::PanelObserver
{
public:
	enum SelectionModification { RemoveFromSelection, AddToSelection };

	struct ElementInfo {
		GS::UniString	guidStr;
		GS::UniString	typeName;
		GS::UniString	elemID;
	};

protected:
	enum {
		BrowserId = 1
	};

	DG::Browser		browser;

	void InitBrowserControl ();
	void RegisterACAPIJavaScriptObject ();
	void UpdateSelectedElementsOnHTML ();
	void SetMenuItemCheckedState (bool);

	virtual void PanelResized (const DG::PanelResizeEvent& ev) override;
	virtual	void PanelCloseRequested (const DG::PanelCloseRequestEvent& ev, bool* accepted) override;

	static GS::Array<BrowserRepl::ElementInfo> GetSelectedElements ();
	static void ModifySelection (const GS::UniString& elemGuidStr, SelectionModification modification);

	static GSErrCode __ACENV_CALL	PaletteControlCallBack (Int32 paletteId, API_PaletteMessageID messageID, GS::IntPtr param);

	static GS::Ref<BrowserRepl> instance;

	BrowserRepl ();

public:
	virtual ~BrowserRepl ();

	static bool				HasInstance ();
	static void				CreateInstance ();
	static BrowserRepl&	GetInstance ();
	static void				DestroyInstance ();

	void Show ();
	void Hide ();

	static GSErrCode				RegisterPaletteControlCallBack ();
	static GSErrCode __ACENV_CALL	SelectionChangeHandler (const API_Neig*);
};

#endif // BROWSERREPL_HPP
