// *****************************************************************************
// Source code for the Browser Control Test Add-On
// *****************************************************************************

// =============================================================================
//
// API Includes
//
// =============================================================================

#include	"APIEnvir.h"
#include	"ACAPinc.h"		// also includes APIdefs.h
#include	"BrowserRepl.hpp"
#include    "HelpPalette.hpp"  

// -----------------------------------------------------------------------------
// Show or Hide Browser Palette
// -----------------------------------------------------------------------------

static void	ShowOrHideBrowserRepl ()
{
	if (BrowserRepl::HasInstance () && BrowserRepl::GetInstance ().IsVisible ()) {
		BrowserRepl::GetInstance ().Hide ();
	} else {
		if (!BrowserRepl::HasInstance ())
			BrowserRepl::CreateInstance ();
		BrowserRepl::GetInstance ().Show ();
	}
}

// -----------------------------------------------------------------------------
// MenuCommandHandler
//		called to perform the user-asked command
// -----------------------------------------------------------------------------

GSErrCode __ACENV_CALL MenuCommandHandler (const API_MenuParams *menuParams)
{
	switch (menuParams->menuItemRef.menuResID) {
		case BrowserReplMenuResId:
			switch (menuParams->menuItemRef.itemIndex) {
				case BrowserReplMenuItemIndex:
					ShowOrHideBrowserRepl ();
					break;
			}
			break;
	}

	return NoError;
}


// =============================================================================
//
// Required functions
//
// =============================================================================

// -----------------------------------------------------------------------------
// Dependency definitions
// -----------------------------------------------------------------------------

API_AddonType	__ACDLL_CALL	CheckEnvironment (API_EnvirParams* envir)
{
	RSGetIndString (&envir->addOnInfo.name, 32000, 1, ACAPI_GetOwnResModule ());
	RSGetIndString (&envir->addOnInfo.description, 32000, 2, ACAPI_GetOwnResModule ());

	return APIAddon_Preload;
}		// CheckEnvironment


// -----------------------------------------------------------------------------
// Interface definitions
// -----------------------------------------------------------------------------

GSErrCode	__ACDLL_CALL	RegisterInterface (void)
{
	GSErrCode err = ACAPI_MenuItem_RegisterMenu (BrowserReplMenuResId, 0, MenuCode_UserDef, MenuFlag_Default);
	if (DBERROR (err != NoError))
		return err;

	return err;
}		// RegisterInterface


// -----------------------------------------------------------------------------
// Initialize
//		called after the Add-On has been loaded into memory
// -----------------------------------------------------------------------------

GSErrCode __ACENV_CALL Initialize ()
{
    // 1) Меню
    GSErrCode err = ACAPI_MenuItem_InstallMenuHandler (BrowserReplMenuResId, MenuCommandHandler);
    if (DBERROR (err != NoError))
        return err;

    // 2) Нотификация выбора
    err = ACAPI_Notification_CatchSelectionChange (BrowserRepl::SelectionChangeHandler);
    if (DBERROR (err != NoError))
        return err;

    // 3) Регистрация модельных окон (палитр) — аккумулируем ошибки
    GSErrCode palErr = NoError;
    palErr |= BrowserRepl::RegisterPaletteControlCallBack ();
    palErr |= HelpPalette::RegisterPaletteControlCallBack ();

    if (DBERROR (palErr != NoError))
        return palErr;

    return NoError;
}	// Initialize


// -----------------------------------------------------------------------------
// FreeData
//		called when the Add-On is going to be unloaded
// -----------------------------------------------------------------------------

GSErrCode __ACENV_CALL	FreeData (void)
{
	return NoError;
}		// FreeData
