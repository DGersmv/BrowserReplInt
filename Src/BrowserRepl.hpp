#ifndef BROWSERREPL_HPP
#define BROWSERREPL_HPP

#include "DGModule.hpp"
#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ResourceIDs.hpp"
#include "DGDefs.h"
#include "DGBrowser.hpp"


// Класс BrowserRepl управляет палитрой и встроенным браузером
class BrowserRepl : public DG::Palette, public DG::PanelObserver {
public:
    BrowserRepl();
    ~BrowserRepl();

    static bool HasInstance();
    static void CreateInstance();
    static BrowserRepl& GetInstance();
    static void DestroyInstance();

    void Show();
    void Hide();
    void InitBrowserControl();

    void UpdateSelectedElementsOnHTML();
    static GSErrCode __ACENV_CALL SelectionChangeHandler(const API_Neig*);
    static GSErrCode __ACENV_CALL PaletteControlCallBack(Int32 referenceID, API_PaletteMessageID messageID, GS::IntPtr param);

    static GSErrCode RegisterPaletteControlCallBack();

    // --- Новый публичный метод для логов ---
    void LogToBrowser(const GS::UniString& msg);

private:
    static GS::Ref<BrowserRepl> instance;
    DG::Browser browser;   // приватный браузер

    void RegisterACAPIJavaScriptObject();
    void SetMenuItemCheckedState(bool isChecked);

    // DG overrides
    void PanelResized(const DG::PanelResizeEvent& ev) override;
    void PanelCloseRequested(const DG::PanelCloseRequestEvent& ev, bool* accepted) override;
};

#endif // BROWSERREPL_HPP
