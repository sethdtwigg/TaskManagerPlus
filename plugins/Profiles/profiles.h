/*
 * TaskManagerPlus Profiles plugin.
 *
 * Named setting profiles (Work, Gaming ...): each stores which Process Rules are on or off, the start priority of those
 * rules and the theme mode. Switch from System > Profiles or the Ctrl+K palette; "Revert" returns to what was set before the
 * last switch. Profiles are stored in tmp-profiles.json and can be exported / imported as JSON.
 */

#ifndef _PROFILES_H_
#define _PROFILES_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include <json.h>
#include "resource.h"
#include <processrulesintf.h>
#include <commandsintf.h>
#include "profilecore.h"

#define PLUGIN_NAME TMP_COMMANDS_PROFILES_PLUGIN_NAME
#define SETTING_NAME_TEST_NAME (PLUGIN_NAME L".TestName") // TaskManagerPlus: profile name used by the test message ops
#define SETTING_NAME_TEST_FILE (PLUGIN_NAME L".TestFile") // TaskManagerPlus: import/export path used by the test message ops
#define PROFILES_FILE_NAME L"tmp-profiles.json"
#define PROFILES_FILE_VERSION 1
#define COMMAND_MESSAGE_NAME L"TaskManagerPlus.Profiles.Command" // wParam: see main.c

#define PR_COMMAND_REVERT 1
#define PR_COMMAND_APPLY_BASE 100 // + index of the profile

extern PPH_PLUGIN PluginInstance;
extern PF_STORE PrStore;
extern TMP_COMMANDS_INTERFACE PrCommandsInterface;

// store.c

VOID PrLoad(
    VOID
    );

VOID PrSave(
    VOID
    );

NTSTATUS PrExportToFile(
    _In_ PPH_STRING FileName,
    _In_opt_ PCWSTR OnlyProfile
    );

/** Imports profiles (replacing the ones with the same name). Imported / Skipped receive the counts. */
NTSTATUS PrImportFromFile(
    _In_ PPH_STRING FileName,
    _Out_ PULONG Imported,
    _Out_ PULONG Skipped
    );

// actions (main.c)

/** Captures the current rules and theme as a profile. Returns FALSE on a bad name or a full store. */
BOOLEAN PrSaveCurrentAs(
    _In_ PCWSTR Name,
    _In_ unsigned CaptureFlags
    );

/** Applies a profile (plan, rules, theme, save, log + notification). Returns FALSE when it does not exist. */
BOOLEAN PrApplyProfile(
    _In_ PCWSTR Name
    );

/** Returns to the settings from before the last switch. FALSE when there is nothing to revert. */
BOOLEAN PrRevert(
    VOID
    );

// ui.c

VOID PrShowSaveDialog(
    _In_ HWND OwnerWindow
    );

VOID PrShowManageDialog(
    _In_ HWND OwnerWindow
    );

#endif
