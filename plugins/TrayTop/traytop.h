/*
 * TaskManagerPlus TrayTop plugin.
 *
 * Adds "Top processes" to the tray icon's right-click menu: the N heaviest processes by CPU and by memory, each with a
 * submenu of one-click actions (End task, Efficiency mode, Open file location). N is configurable (1 to 15, default 5)
 * under Options > Tray top processes.
 */

#ifndef _TRAYTOP_H_
#define _TRAYTOP_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"
#include "topn.h"

#define PLUGIN_NAME L"TaskManagerPlus.TrayTop"
#define SETTING_NAME_ENABLED (PLUGIN_NAME L".Enabled")
#define SETTING_NAME_COUNT (PLUGIN_NAME L".Count")
#define SHOW_MESSAGE_NAME L"TaskManagerPlus.TrayTop.Show"     // tests: build and show the menu at wParam,lParam = x,y (screen)
#define ACTION_MESSAGE_NAME L"TaskManagerPlus.TrayTop.Action" // tests: wParam = pid, lParam = TT_ACTION

typedef enum _TT_ACTION
{
    TtActionEnd = 1,
    TtActionEfficiency = 2,
    TtActionOpenLocation = 3
} TT_ACTION;

extern PPH_PLUGIN PluginInstance;

// menu.c

/** Adds the "Top processes" submenu to the tray menu, before Exit. */
VOID TtAddToTrayMenu(
    _In_ PPH_PLUGIN_MENU_INFORMATION MenuInfo
    );

/** Runs a menu item of ours. Returns TRUE when Id was one of ours. */
BOOLEAN TtHandleMenuItem(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    );

/** The action itself, shared by the menu and the test message. Returns FALSE when the process is gone. */
BOOLEAN TtRunAction(
    _In_ ULONG Pid,
    _In_ TT_ACTION Action,
    _In_ HWND OwnerWindow
    );

/** Builds the same menu the tray shows and pops it up at the given screen position (tests). */
VOID TtShowTestMenu(
    _In_ HWND OwnerWindow,
    _In_ LONG X,
    _In_ LONG Y
    );

VOID TtReleaseMenuReferences(
    VOID
    );

// options.c

INT_PTR CALLBACK TtOptionsDlgProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    );

#endif
