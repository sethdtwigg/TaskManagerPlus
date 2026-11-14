/*
 * TaskManagerPlus TrayTop plugin: registration, the tray menu hook and the test messages.
 */

#include "traytop.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static PH_CALLBACK_REGISTRATION IconMenuInitializingCallbackRegistration;
static PH_CALLBACK_REGISTRATION OptionsCallbackRegistration;
static PPH_MESSAGE_LOOP_FILTER_ENTRY FilterEntry = NULL;
static UINT ShowMessage = 0;
static UINT ActionMessage = 0;

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI MessageLoopFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    if (ShowMessage && Message->message == ShowMessage)
    {
        TtShowTestMenu(SystemInformer_GetWindowHandle(), (LONG)Message->wParam, (LONG)Message->lParam);
        return TRUE;
    }

    if (ActionMessage && Message->message == ActionMessage)
    {
        TtRunAction((ULONG)Message->wParam, (TT_ACTION)Message->lParam, SystemInformer_GetWindowHandle());
        return TRUE;
    }

    return FALSE;
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI IconMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (Parameter)
        TtAddToTrayMenu((PPH_PLUGIN_MENU_INFORMATION)Parameter);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;

    if (menuItem)
        TtHandleMenuItem(menuItem->Id, menuItem->OwnerWindow);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ShowOptionsCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_OPTIONS_POINTERS optionsEntry = (PPH_PLUGIN_OPTIONS_POINTERS)Parameter;

    optionsEntry->CreateSection(
        L"Tray top processes",
        PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_OPTIONS),
        TtOptionsDlgProc,
        NULL
        );
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    ShowMessage = RegisterWindowMessage(SHOW_MESSAGE_NAME);
    ActionMessage = RegisterWindowMessage(ACTION_MESSAGE_NAME);
    FilterEntry = PhRegisterMessageLoopFilter(MessageLoopFilter, NULL);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (FilterEntry)
    {
        PhUnregisterMessageLoopFilter(FilterEntry);
        FilterEntry = NULL;
    }

    TtReleaseMenuReferences();
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
            { IntegerSettingType, SETTING_NAME_ENABLED, L"1" },
            { IntegerSettingType, SETTING_NAME_COUNT, L"5" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Tray Top Processes";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Adds the busiest processes by CPU and by memory to the tray icon's right-click menu, each with one-click End task, Efficiency mode and Open file location.";
        info->HasOptions = FALSE;

        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackLoad), LoadCallback, NULL, &PluginLoadCallbackRegistration);
        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackUnload), UnloadCallback, NULL, &PluginUnloadCallbackRegistration);
        PhRegisterCallback(PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem), MenuItemCallback, NULL, &PluginMenuItemCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackIconMenuInitializing), IconMenuInitializingCallback, NULL, &IconMenuInitializingCallbackRegistration);
        PhRegisterCallback(PhGetGeneralCallback(GeneralCallbackOptionsWindowInitializing), ShowOptionsCallback, NULL, &OptionsCallbackRegistration);

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
