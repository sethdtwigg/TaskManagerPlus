/*
 * TaskManagerPlus StartupManager plugin.
 *
 * Lists everything that runs at logon (registry Run/RunOnce keys, startup
 * folders and logon/boot scheduled tasks) in a main-window "Startup" tab and
 * lets the user enable, disable or delete entries.
 */

#ifndef _STARTUPMANAGER_H_
#define _STARTUPMANAGER_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"

#define PLUGIN_NAME L"TaskManagerPlus.StartupManager"
#define SETTING_NAME_TREE_LIST_COLUMNS (PLUGIN_NAME L".TreeListColumns")
#define SETTING_NAME_TREE_LIST_SORT (PLUGIN_NAME L".TreeListSort")

extern PPH_PLUGIN PluginInstance;

// Where a startup entry comes from. Each row records this so the entry can be
// edited in place (StartupApproved value, shortcut file or scheduled task).

typedef enum _SM_SOURCE_TYPE
{
    SmSourceRegistryHklm,
    SmSourceRegistryHkcu,
    SmSourceStartupFolderUser,
    SmSourceStartupFolderCommon,
    SmSourceScheduledTask,
} SM_SOURCE_TYPE;

typedef enum _SM_IMPACT
{
    SmImpactUnknown,
    SmImpactLow,
    SmImpactMedium,
    SmImpactHigh,
} SM_IMPACT;

typedef enum _SM_COLUMN
{
    SM_COLUMN_NAME,
    SM_COLUMN_STATUS,
    SM_COLUMN_TYPE,
    SM_COLUMN_PUBLISHER,
    SM_COLUMN_COMMAND,
    SM_COLUMN_LOCATION,
    SM_COLUMN_IMPACT,
    SM_COLUMN_MAXIMUM
} SM_COLUMN;

typedef struct _SM_STARTUP_ITEM
{
    PH_TREENEW_NODE Node;
    PH_STRINGREF TextCache[SM_COLUMN_MAXIMUM];

    ULONG Index;
    SM_SOURCE_TYPE Source;
    BOOLEAN Enabled;
    BOOLEAN CanToggle;
    BOOLEAN Wow64;
    BOOLEAN RunOnce;
    SM_IMPACT Impact;
    ULONG64 FileSize;

    PPH_STRING Name; // registry value name, shortcut base name or task name
    PPH_STRING Command; // full command line or resolved target with arguments
    PPH_STRING Location; // registry key path (HKLM\...), folder path or task folder path
    PPH_STRING FileName; // resolved target executable (Win32 path), may be NULL
    PPH_STRING Publisher; // CompanyName from the target's version info, may be NULL

    // Registry entries: the key path relative to the hive root that holds the value.
    // Folder entries: the full path of the shortcut or executable in the folder.
    // Scheduled tasks: the full task path ("\Folder\Name").
    PPH_STRING ObjectPath;

    // StartupApproved subkey name ("Run", "Run32" or "StartupFolder") and the
    // value name under it (registry value name or shortcut file name).
    PH_STRINGREF ApprovedKeyName;
    PPH_STRING ApprovedValueName;
} SM_STARTUP_ITEM, *PSM_STARTUP_ITEM;

// startupenum.c

VOID SmEnumerateStartupItems(
    _Inout_ PPH_LIST List
    );

VOID SmFreeStartupItem(
    _In_ PSM_STARTUP_ITEM Item
    );

NTSTATUS SmSetStartupItemEnabled(
    _In_ PSM_STARTUP_ITEM Item,
    _In_ BOOLEAN Enable
    );

NTSTATUS SmDeleteStartupItem(
    _In_ PSM_STARTUP_ITEM Item
    );

PCPH_STRINGREF SmGetSourceTypeString(
    _In_ PSM_STARTUP_ITEM Item
    );

PCPH_STRINGREF SmGetImpactString(
    _In_ SM_IMPACT Impact
    );

// startuptab.c

VOID SmInitializeStartupTab(
    VOID
    );

#endif
