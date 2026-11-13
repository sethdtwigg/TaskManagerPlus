/*
 * TaskManagerPlus ResourceTrends plugin: registration, callbacks and options.
 */

#include "resourcetrends.h"
#include <resourcetrendsintf.h>

extern RESOURCE_TRENDS_INTERFACE TrsInterface; // intf.c

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginShowOptionsCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    TrsStoreInitialize();
    TrsStoreLoad();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    TrsStoreFlush();
    TrsTrackerShutdown();
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
        L"Resource Trends",
        PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_OPTIONS),
        TrsOptionsDlgProc,
        NULL
        );
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainWindowShowingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    TrsInitializeTab();
}

/** Once per refresh, main thread. TrsCollect decides whether the sample interval has elapsed. */
_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    static BOOLEAN selected = FALSE;

    if (!selected)
    {
        selected = TRUE;

        if (PhGetIntegerSetting(SETTING_NAME_TEST_SELECT_TAB))
            TrsSelectTab();
    }

    TrsCollect();
}

static ULONG TrsOptionValue(
    _In_ HWND WindowHandle,
    _In_ ULONG ControlId,
    _In_ ULONG Minimum,
    _In_ ULONG Maximum
    )
{
    ULONG value = PhGetDialogItemValue(WindowHandle, ControlId);

    if (value < Minimum)
        return Minimum;
    if (value > Maximum)
        return Maximum;
    return value;
}

INT_PTR CALLBACK TrsOptionsDlgProc(
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
            LK_CONFIG config;

            TrsGetLeakConfig(&config);
            PhSetDialogItemValue(WindowHandle, IDC_SAMPLE_SECONDS, TrsGetSampleSeconds(), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_RETENTION_HOURS, TrsGetRetentionHours(), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_MAX_FILE_MB, TrsGetMaxFileMB(), FALSE);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_PERSIST), PhGetIntegerSetting(SETTING_NAME_PERSIST) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_LEAK_ENABLED), PhGetIntegerSetting(SETTING_NAME_LEAK_ENABLED) ? BST_CHECKED : BST_UNCHECKED);
            PhSetDialogItemValue(WindowHandle, IDC_LEAK_WINDOW, (ULONG)(config.WindowSeconds / 60), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_LEAK_GROWTH, (ULONG)(config.MinGrowthKB / 1024), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_LEAK_PERCENT, config.MinGrowthPercent, FALSE);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_LEAK_NOTIFY), PhGetIntegerSetting(SETTING_NAME_LEAK_NOTIFY) ? BST_CHECKED : BST_UNCHECKED);
        }
        break;
    case WM_DESTROY:
        {
            // Sections are destroyed when the options window closes; persist here like the other plugins do.
            PhSetIntegerSetting(SETTING_NAME_SAMPLE_SECONDS, TrsOptionValue(WindowHandle, IDC_SAMPLE_SECONDS, 1, 3600));
            PhSetIntegerSetting(SETTING_NAME_RETENTION_HOURS, TrsOptionValue(WindowHandle, IDC_RETENTION_HOURS, 1, 2160));
            PhSetIntegerSetting(SETTING_NAME_MAX_FILE_MB, TrsOptionValue(WindowHandle, IDC_MAX_FILE_MB, 1, 64));
            PhSetIntegerSetting(SETTING_NAME_PERSIST, Button_GetCheck(GetDlgItem(WindowHandle, IDC_PERSIST)) == BST_CHECKED);
            PhSetIntegerSetting(SETTING_NAME_LEAK_ENABLED, Button_GetCheck(GetDlgItem(WindowHandle, IDC_LEAK_ENABLED)) == BST_CHECKED);
            PhSetIntegerSetting(SETTING_NAME_LEAK_WINDOW_SECONDS, TrsOptionValue(WindowHandle, IDC_LEAK_WINDOW, 1, 1440) * 60);
            PhSetIntegerSetting(SETTING_NAME_LEAK_MIN_GROWTH_MB, TrsOptionValue(WindowHandle, IDC_LEAK_GROWTH, 1, 1048576));
            PhSetIntegerSetting(SETTING_NAME_LEAK_MIN_PERCENT, TrsOptionValue(WindowHandle, IDC_LEAK_PERCENT, 0, 1000));
            PhSetIntegerSetting(SETTING_NAME_LEAK_NOTIFY, Button_GetCheck(GetDlgItem(WindowHandle, IDC_LEAK_NOTIFY)) == BST_CHECKED);
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
            { IntegerSettingType, SETTING_NAME_SAMPLE_SECONDS, L"1e" }, // 30
            { IntegerSettingType, SETTING_NAME_RETENTION_HOURS, L"a8" }, // 168
            { IntegerSettingType, SETTING_NAME_MAX_FILE_MB, L"8" },
            { IntegerSettingType, SETTING_NAME_PERSIST, L"1" },
            { IntegerSettingType, SETTING_NAME_LEAK_ENABLED, L"1" },
            { IntegerSettingType, SETTING_NAME_LEAK_WINDOW_SECONDS, L"708" }, // 1800
            { IntegerSettingType, SETTING_NAME_LEAK_MIN_GROWTH_MB, L"64" }, // 100
            { IntegerSettingType, SETTING_NAME_LEAK_MIN_PERCENT, L"a" }, // 10
            { IntegerSettingType, SETTING_NAME_LEAK_NOTIFY, L"1" },
            { StringSettingType, SETTING_NAME_TEST_OUTPUT, L"" },
            { IntegerSettingType, SETTING_NAME_TEST_SELECT_TAB, L"0" },
            { IntegerSettingType, SETTING_NAME_RANGE_INDEX, L"1" },
            { IntegerSettingType, SETTING_NAME_VIEW_INDEX, L"0" },
            { StringSettingType, SETTING_NAME_USAGE_COLUMNS, L"" },
            { IntegerPairSettingType, SETTING_NAME_USAGE_SORT, L"2,2" },
            { StringSettingType, SETTING_NAME_LEAK_COLUMNS, L"" },
            { IntegerPairSettingType, SETTING_NAME_LEAK_SORT, L"3,2" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Resource Trends";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Records system and per-process CPU, memory and commit history on disk, shows sparklines and a"
            L" \"what used the CPU or memory at this time\" lookup on a Trends tab, and flags processes whose private bytes grow steadily.";
        info->HasOptions = TRUE;
        info->Interface = &TrsInterface; // TaskManagerPlus: usage table for SessionReports

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

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
