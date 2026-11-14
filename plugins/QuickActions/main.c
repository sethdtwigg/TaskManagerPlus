/*
 * TaskManagerPlus QuickActions plugin: registration and the Ctrl+K hook.
 *
 * The hotkey is a message loop filter (PhRegisterMessageLoopFilter), so the core needs no accelerator entry.
 * ToolStatus used to own Ctrl+K (focus the search box); its accelerator was moved to Ctrl+E.
 */

#include "quickactions.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PPH_MESSAGE_LOOP_FILTER_ENTRY FilterEntry = NULL;
static UINT OpenMessage = 0;

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI MessageLoopFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    // The shortcut is configurable (Options > Shortcuts, default Ctrl+K); Alt combinations arrive as WM_SYSKEYDOWN.
    if ((Message->message == WM_KEYDOWN || Message->message == WM_SYSKEYDOWN) && PhShortcutMatches("quickactions", (ULONG)Message->wParam))
    {
        BOOLEAN ours;

        ours = QaIsPaletteWindow(GetAncestor(Message->hwnd, GA_ROOT));

        if (ours)
        {
            QaClosePalette();
            return TRUE;
        }

        if (Message->hwnd == SystemInformer_GetWindowHandle() || IsChild(SystemInformer_GetWindowHandle(), Message->hwnd))
        {
            QaOpenPalette();
            return TRUE;
        }

        return FALSE;
    }

    if (OpenMessage && Message->message == OpenMessage)
    {
        if (QaIsPaletteOpen())
            QaClosePalette();
        else
            QaOpenPalette();

        return TRUE;
    }

    return FALSE;
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    OpenMessage = RegisterWindowMessage(OPEN_MESSAGE_NAME);
    FilterEntry = PhRegisterMessageLoopFilter(MessageLoopFilter, NULL);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    QaClosePalette();

    if (FilterEntry)
    {
        PhUnregisterMessageLoopFilter(FilterEntry);
        FilterEntry = NULL;
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
            { StringSettingType, SETTING_NAME_RECENT, L"" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Quick Actions";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Ctrl+K command palette: fuzzy search over processes, services and startup items, and run an action on the result (end task, suspend, efficiency mode, priority, open file location and more).";
        info->HasOptions = FALSE;

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

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
