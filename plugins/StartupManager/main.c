/*
 * TaskManagerPlus StartupManager plugin.
 *
 * Plugin registration and callbacks.
 */

#include "startupmanager.h"

PPH_PLUGIN PluginInstance = NULL;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    NOTHING;
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    NOTHING;
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainWindowShowingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    SmInitializeStartupTab();
}

// TaskManagerPlus: interface for the QuickActions palette (see plugins\include\startupmanagerintf.h).

static PPH_LIST NTAPI SmIntfEnumerate(
    VOID
    )
{
    PPH_LIST items = PhCreateList(64);
    PPH_LIST entries = PhCreateList(64);
    ULONG i;

    SmEnumerateStartupItems(items);

    for (i = 0; i < items->Count; i++)
    {
        PSM_STARTUP_ITEM item = items->Items[i];
        PSTARTUP_ENTRY_INFO entry = PhAllocateZero(sizeof(STARTUP_ENTRY_INFO));
        PCPH_STRINGREF type = SmGetSourceTypeString(item);

        PhSetReference(&entry->Name, item->Name);
        PhSetReference(&entry->Command, item->Command);
        PhSetReference(&entry->FileName, item->FileName);
        entry->Type = type ? PhCreateString2(type) : NULL;
        entry->Enabled = item->Enabled;
        entry->CanToggle = item->CanToggle;
        entry->Source = item->Source;
        PhSetReference(&entry->ObjectPath, item->ObjectPath);
        PhSetReference(&entry->ApprovedValueName, item->ApprovedValueName);
        PhAddItemList(entries, entry);
        SmFreeStartupItem(item);
    }

    PhDereferenceObject(items);
    return entries;
}

static VOID NTAPI SmIntfFreeEntries(
    _In_ PPH_LIST Entries
    )
{
    ULONG i;

    for (i = 0; i < Entries->Count; i++)
    {
        PSTARTUP_ENTRY_INFO entry = Entries->Items[i];

        PhClearReference(&entry->Name);
        PhClearReference(&entry->Command);
        PhClearReference(&entry->FileName);
        PhClearReference(&entry->Type);
        PhClearReference(&entry->ObjectPath);
        PhClearReference(&entry->ApprovedValueName);
        PhFree(entry);
    }

    PhDereferenceObject(Entries);
}

static BOOLEAN SmIntfEqual(
    _In_opt_ PPH_STRING A,
    _In_opt_ PPH_STRING B
    )
{
    if (!A || !B)
        return A == B;

    return PhEqualString(A, B, FALSE);
}

static NTSTATUS NTAPI SmIntfSetEnabled(
    _In_ PSTARTUP_ENTRY_INFO Entry,
    _In_ BOOLEAN Enable
    )
{
    PPH_LIST items = PhCreateList(64);
    NTSTATUS status = STATUS_NOT_FOUND;
    ULONG i;

    SmEnumerateStartupItems(items);

    for (i = 0; i < items->Count; i++)
    {
        PSM_STARTUP_ITEM item = items->Items[i];

        if (status == STATUS_NOT_FOUND &&
            (ULONG)item->Source == Entry->Source &&
            SmIntfEqual(item->ObjectPath, Entry->ObjectPath) &&
            SmIntfEqual(item->Name, Entry->Name) &&
            SmIntfEqual(item->ApprovedValueName, Entry->ApprovedValueName))
        {
            status = SmSetStartupItemEnabled(item, Enable);
        }

        SmFreeStartupItem(item);
    }

    PhDereferenceObject(items);
    return status;
}

static STARTUP_MANAGER_INTERFACE SmInterface =
{
    STARTUP_MANAGER_INTERFACE_VERSION,
    SmIntfEnumerate,
    SmIntfFreeEntries,
    SmIntfSetEnabled
};

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        PPH_PLUGIN_INFORMATION info;
        PH_SETTING_CREATE settings[] =
        {
            { StringSettingType, SETTING_NAME_TREE_LIST_COLUMNS, L"" },
            { IntegerPairSettingType, SETTING_NAME_TREE_LIST_SORT, L"0,1" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Startup Manager";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Adds a Startup tab listing registry Run keys, startup folder items and logon scheduled tasks, with enable, disable and delete.";
        info->HasOptions = FALSE;
        info->Interface = &SmInterface; // TaskManagerPlus: startup items for the QuickActions palette

        PhRegisterCallback(
            PhGetPluginCallback(PluginInstance, PluginCallbackLoad),
            LoadCallback,
            NULL,
            &PluginLoadCallbackRegistration
            );
        PhRegisterCallback(
            PhGetPluginCallback(PluginInstance, PluginCallbackUnload),
            UnloadCallback,
            NULL,
            &PluginUnloadCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackMainWindowShowing),
            MainWindowShowingCallback,
            NULL,
            &MainWindowShowingCallbackRegistration
            );

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
