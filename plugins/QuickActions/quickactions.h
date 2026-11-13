/*
 * TaskManagerPlus QuickActions plugin.
 *
 * Ctrl+K opens a command palette: live fuzzy search over running processes, services, startup items and the
 * actions that apply to them (end task, end tree, suspend, resume, efficiency mode, priority, open file location,
 * properties, go to process, start/stop service, enable/disable startup item). Enter runs the highlighted action
 * with the same confirmations the rest of the UI shows.
 */

#ifndef _QUICKACTIONS_H_
#define _QUICKACTIONS_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"
#include <startupmanagerintf.h>
#include <commandsintf.h>
#include "fuzzy.h"

#define PLUGIN_NAME L"TaskManagerPlus.QuickActions"
#define SETTING_NAME_RECENT (PLUGIN_NAME L".Recent")
#define OPEN_MESSAGE_NAME L"TaskManagerPlus.QuickActions.Open" // registered window message; tests post it instead of Ctrl+K

extern PPH_PLUGIN PluginInstance;

VOID QaOpenPalette(
    VOID
    );

VOID QaClosePalette(
    VOID
    );

BOOLEAN QaIsPaletteWindow(
    _In_ HWND WindowHandle
    );

BOOLEAN QaIsPaletteOpen(
    VOID
    );

#endif
