/*
 * TaskManagerPlus Process History plugin: registration, callbacks and options.
 */

#include "processhistory.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginShowOptionsCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessAddedCallbackRegistration; // TaskManagerPlus: lets the capture know which processes the list showed

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    HistStoreInitialize();
    HistStoreLoad();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    CapShutdown();
    HistStoreFlush();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ShowOptionsCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_OPTIONS_POINTERS optionsEntry = (PPH_PLUGIN_OPTIONS_POINTERS)Parameter;

    if (!optionsEntry)
        return;

    optionsEntry->CreateSection(
        L"Process History",
        PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_OPTIONS),
        HistOptionsDlgProc,
        NULL
        );
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainWindowShowingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    HistInitializeTab();

    if (PhGetIntegerSetting(SETTING_NAME_SELF_TEST)) // TaskManagerPlus: deterministic thinning check, see HistStoreSelfTest
    {
        PhSetIntegerSetting(SETTING_NAME_SELF_TEST, 0);
        HistStoreSelfTest();
    }
}

/**
 * Fires once per refresh on the main thread after the process tree has been
 * updated. The store decides whether enough time has passed for a snapshot.
 */
_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    CapUpdate();
    HistStoreCollect();
}

/** [process provider thread] */
_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessAddedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PROCESS_ITEM processItem = Parameter;

    if (processItem)
        CapMarkSeen(HandleToUlong(processItem->ProcessId), processItem->CreateTime.QuadPart);
}

static ULONG HistClampSetting(
    _In_ ULONG Value,
    _In_ ULONG Minimum,
    _In_ ULONG Maximum
    )
{
    if (Value < Minimum)
        return Minimum;
    if (Value > Maximum)
        return Maximum;
    return Value;
}

INT_PTR CALLBACK HistOptionsDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PhSetDialogItemValue(WindowHandle, IDC_INTERVAL, HistGetIntervalSeconds(), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_RETENTION, HistGetRetentionHours(), FALSE);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_PERSIST), PhGetIntegerSetting(SETTING_NAME_PERSIST_TO_DISK) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_CAPTURE), PhGetIntegerSetting(SETTING_NAME_CAPTURE) ? BST_CHECKED : BST_UNCHECKED);
        }
        break;
    case WM_DESTROY:
        {
            // Options sections are destroyed when the options window closes
            // (the framework applies all sections on OK); persist the values
            // here as the other plugins do. The collector re-reads the
            // settings on its next tick.
            PhSetIntegerSetting(
                SETTING_NAME_INTERVAL_SECONDS,
                HistClampSetting(PhGetDialogItemValue(WindowHandle, IDC_INTERVAL), HIST_MIN_INTERVAL_SECONDS, HIST_MAX_INTERVAL_SECONDS)
                );
            PhSetIntegerSetting(
                SETTING_NAME_RETENTION_HOURS,
                HistClampSetting(PhGetDialogItemValue(WindowHandle, IDC_RETENTION), HIST_MIN_RETENTION_HOURS, HIST_MAX_RETENTION_HOURS)
                );
            PhSetIntegerSetting(
                SETTING_NAME_PERSIST_TO_DISK,
                Button_GetCheck(GetDlgItem(WindowHandle, IDC_PERSIST)) == BST_CHECKED
                );
            PhSetIntegerSetting(
                SETTING_NAME_CAPTURE,
                Button_GetCheck(GetDlgItem(WindowHandle, IDC_CAPTURE)) == BST_CHECKED
                );
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
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
            { IntegerSettingType, SETTING_NAME_INTERVAL_SECONDS, L"a" }, // 10
            { IntegerSettingType, SETTING_NAME_RETENTION_HOURS, L"18" }, // 24
            { IntegerSettingType, SETTING_NAME_PERSIST_TO_DISK, L"1" },
            { IntegerSettingType, SETTING_NAME_SELF_TEST, L"0" },
            { IntegerSettingType, SETTING_NAME_CAPTURE, L"1" },
            { IntegerSettingType, SETTING_NAME_CAPTURE_SELF_TEST, L"0" },
            { StringSettingType, SETTING_NAME_TREE_LIST_COLUMNS, L"" },
            { IntegerPairSettingType, SETTING_NAME_TREE_LIST_SORT, L"0,1" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Process History";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Records periodic snapshots of the process list and adds a History tab"
            L" for scrubbing back in time to see which processes were running, started or exited."
            L" With administrator rights it also records processes that start and exit between two refreshes (ETW).";
        info->HasOptions = TRUE;
        info->Interface = &CapInterface; // TaskManagerPlus: short-lived process records for ProcessRules

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
            PhGetGeneralCallback(GeneralCallbackMainWindowShowing),
            MainWindowShowingCallback,
            NULL,
            &MainWindowShowingCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
            ProcessesUpdatedCallback,
            NULL,
            &ProcessesUpdatedCallbackRegistration
            );

        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackProcessProviderAddedEvent),
            ProcessAddedCallback,
            NULL,
            &ProcessAddedCallbackRegistration
            );

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
