/*
 * TaskManagerPlus ProcessRules plugin - registration
 */

#include "processrules.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginShowOptionsCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessMenuInitializingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    TmpEngineInitialize();
    TmpLoadRules();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    TmpSaveRules();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ShowOptionsCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    PPH_PLUGIN_OPTIONS_POINTERS optionsEntry = (PPH_PLUGIN_OPTIONS_POINTERS)Parameter;

    optionsEntry->CreateSection(
        L"Process Rules",
        PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_OPTIONS),
        TmpOptionsDlgProc,
        NULL
        );
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainWindowShowingCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    TmpInitializeRulesTab();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessMenuInitializingCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_PROCESS_ITEM processItem;
    PPH_EMENU_ITEM rulesMenu;
    PPH_EMENU_ITEM miscMenuItem;
    PTMP_RULE rule = NULL;
    ULONG insertIndex;

    if (menuInfo->u.Process.NumberOfProcesses != 1)
        return;

    processItem = menuInfo->u.Process.Processes[0];

    if (PH_IS_FAKE_PROCESS_ID(processItem->ProcessId) ||
        processItem->ProcessId == SYSTEM_IDLE_PROCESS_ID ||
        processItem->ProcessId == SYSTEM_PROCESS_ID ||
        !processItem->ProcessName)
    {
        return;
    }

    rule = TmpFindRuleByProgramName(&processItem->ProcessName->sr);

    rulesMenu = PhCreateEMenuItem(0, 0, L"&Rules", NULL, NULL);

    if (rule)
    {
        PhInsertEMenuItem(rulesMenu, PhPluginCreateEMenuItem(PluginInstance, 0, TMP_MENU_EDIT_RULE, L"&Edit rule...", processItem), ULONG_MAX);
        PhInsertEMenuItem(rulesMenu, PhPluginCreateEMenuItem(PluginInstance, 0, TMP_MENU_TOGGLE_RULE, rule->Enabled ? L"&Disable rule" : L"&Enable rule", processItem), ULONG_MAX);
    }
    else
    {
        PhInsertEMenuItem(rulesMenu, PhPluginCreateEMenuItem(PluginInstance, 0, TMP_MENU_CREATE_RULE, L"&Create rule...", processItem), ULONG_MAX);
    }

    // Place the submenu just above Miscellaneous, or at the end if that item is missing.
    insertIndex = ULONG_MAX;

    if (miscMenuItem = PhFindEMenuItem(menuInfo->Menu, 0, NULL, PHAPP_ID_PROCESS_MISCELLANEOUS))
        insertIndex = PhIndexOfEMenuItem(menuInfo->Menu, miscMenuItem);

    PhInsertEMenuItem(menuInfo->Menu, rulesMenu, insertIndex);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MenuItemCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;
    PPH_PROCESS_ITEM processItem = menuItem->Context;

    if (!processItem)
        return;

    switch (menuItem->Id)
    {
    case TMP_MENU_CREATE_RULE:
    case TMP_MENU_EDIT_RULE:
        TmpEditRuleForProcess(menuItem->OwnerWindow, processItem);
        break;
    case TMP_MENU_TOGGLE_RULE:
        {
            PTMP_RULE rule;

            if (processItem->ProcessName && (rule = TmpFindRuleByProgramName(&processItem->ProcessName->sr)))
            {
                rule->Enabled = !rule->Enabled;
                rule->AutoDisabled = FALSE;
                TmpRulesChanged();
            }
        }
        break;
    }
}

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
            { StringSettingType, SETTING_NAME_RULES_COLUMNS, L"" },
            { IntegerPairSettingType, SETTING_NAME_RULES_SORT, L"0,1" },
            { IntegerSettingType, SETTING_NAME_ENGINE_ENABLED, L"1" },
            { IntegerSettingType, SETTING_NAME_NOTIFY, L"1" },
            { IntegerSettingType, SETTING_NAME_LOG, L"1" },
            { IntegerSettingType, SETTING_NAME_GUARD_GLOBAL, L"a" }, // 10 per minute
            { IntegerSettingType, SETTING_NAME_GUARD_RULE, L"5" },
            { IntegerSettingType, SETTING_NAME_DUMPS_PER_HOUR, L"3" },
            { StringSettingType, SETTING_NAME_DUMP_DIRECTORY, L"" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Process Rules";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Per-program rules: set priority, affinity (including P/E-core presets), efficiency mode, block or run a command when a program starts, and watchdogs that act when CPU, memory, I/O, handle or thread usage stays above a threshold.";
        info->HasOptions = TRUE;

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
            PhGetGeneralCallback(GeneralCallbackOptionsWindowInitializing),
            ShowOptionsCallback,
            NULL,
            &PluginShowOptionsCallbackRegistration
            );
        PhRegisterCallback(
            PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem),
            MenuItemCallback,
            NULL,
            &PluginMenuItemCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackMainWindowShowing),
            MainWindowShowingCallback,
            NULL,
            &MainWindowShowingCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackProcessMenuInitializing),
            ProcessMenuInitializingCallback,
            NULL,
            &ProcessMenuInitializingCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
            TmpProcessesUpdatedCallback,
            NULL,
            &ProcessesUpdatedCallbackRegistration
            );

        PhPluginSetObjectExtension(
            PluginInstance,
            EmProcessItemType,
            sizeof(TMP_PROCESS_EXTENSION),
            TmpProcessItemCreateCallback,
            TmpProcessItemDeleteCallback
            );

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
