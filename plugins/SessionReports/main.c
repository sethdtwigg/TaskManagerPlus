/*
 * TaskManagerPlus SessionReports plugin: registration, the System > Export report menu and the palette commands.
 */

#include "sessionreports.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainMenuInitializingCallbackRegistration;
static PPH_MESSAGE_LOOP_FILTER_ENTRY FilterEntry = NULL;
static UINT ExportMessage = 0;

// Menu item ids and palette command ids are the same: kind * 3 + format + 1.
#define SR_COMMAND_ID(Kind, Format) ((ULONG)((Kind) * SrFormatMaximum + (Format) + 1))

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI MessageLoopFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    if (ExportMessage && Message->message == ExportMessage)
    {
        PPH_STRING directory = PhGetStringSetting(SETTING_NAME_TEST_DIRECTORY);

        if (directory->Length && Message->wParam < SrKindMaximum && Message->lParam >= 0 && Message->lParam < SrFormatMaximum)
            SrExportToDirectory((SR_KIND)Message->wParam, (SR_FORMAT)Message->lParam, directory);

        PhDereferenceObject(directory);
        return TRUE;
    }

    return FALSE;
}

static VOID SrRunCommand(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    )
{
    if (Id >= 1 && Id <= SrKindMaximum * SrFormatMaximum)
        SrExportInteractive(OwnerWindow, (SR_KIND)((Id - 1) / SrFormatMaximum), (SR_FORMAT)((Id - 1) % SrFormatMaximum));
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;

    if (menuItem && menuItem->Id)
        SrRunCommand(menuItem->Id, menuItem->OwnerWindow);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_EMENU_ITEM exportMenu;
    ULONG kind;

    if (!menuInfo || menuInfo->u.MainMenu.SubMenuIndex != PH_MENU_ITEM_LOCATION_SYSTEM)
        return;

    exportMenu = PhPluginCreateEMenuItem(PluginInstance, 0, 0, L"Export &report", NULL);

    for (kind = 0; kind < SrKindMaximum; kind++)
    {
        PPH_EMENU_ITEM kindMenu = PhPluginCreateEMenuItem(PluginInstance, 0, 0, (PWSTR)SrGetKindTitle((SR_KIND)kind), NULL);
        ULONG format;

        for (format = 0; format < SrFormatMaximum; format++)
        {
            static PCWSTR formatText[SrFormatMaximum] = { L"As &CSV...", L"As &JSON...", L"As &HTML page..." };

            PhInsertEMenuItem(kindMenu, PhPluginCreateEMenuItem(PluginInstance, 0, SR_COMMAND_ID(kind, format), (PWSTR)formatText[format], NULL), ULONG_MAX);
        }

        PhInsertEMenuItem(exportMenu, kindMenu, ULONG_MAX);
    }

    // Place it above the first separator of the System menu so it sits with the file-like commands.
    PhInsertEMenuItem(menuInfo->Menu, exportMenu, 0);
}

// ---- palette commands

static PPH_LIST NTAPI SrCommandsEnumerate(
    VOID
    )
{
    PPH_LIST commands = PhCreateList(SrKindMaximum * SrFormatMaximum);
    ULONG kind;

    for (kind = 0; kind < SrKindMaximum; kind++)
    {
        ULONG format;

        for (format = 0; format < SrFormatMaximum; format++)
        {
            PTMP_COMMAND_INFO command = PhAllocateZero(sizeof(TMP_COMMAND_INFO));
            static PCWSTR formatName[SrFormatMaximum] = { L"CSV", L"JSON", L"HTML" };

            command->Id = SR_COMMAND_ID(kind, format);
            command->Label = PhFormatString(L"Export %s as %s", SrGetKindName((SR_KIND)kind), formatName[format]);
            command->Detail = PhCreateString(L"Report");
            command->Alias = L"save report session snapshot";
            PhAddItemList(commands, command);
        }
    }

    return commands;
}

static VOID NTAPI SrCommandsFree(
    _In_ PPH_LIST Commands
    )
{
    ULONG i;

    for (i = 0; i < Commands->Count; i++)
    {
        PTMP_COMMAND_INFO command = Commands->Items[i];

        PhClearReference(&command->Label);
        PhClearReference(&command->Detail);
        PhFree(command);
    }

    PhDereferenceObject(Commands);
}

static VOID NTAPI SrCommandsRun(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    )
{
    SrRunCommand(Id, OwnerWindow);
}

TMP_COMMANDS_INTERFACE SrCommandsInterface =
{
    TMP_COMMANDS_INTERFACE_VERSION,
    SrCommandsEnumerate,
    SrCommandsFree,
    SrCommandsRun
};

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    ExportMessage = RegisterWindowMessage(EXPORT_MESSAGE_NAME);
    SrCliInitialize(); // TaskManagerPlus: --report kind path
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
            { StringSettingType, SETTING_NAME_LAST_DIRECTORY, L"" },
            { IntegerSettingType, SETTING_NAME_TREND_HOURS, L"18" }, // 24
            { StringSettingType, SETTING_NAME_TEST_DIRECTORY, L"" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Session Reports";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Exports the process list, startup items, services or the last hours of resource trends to CSV, JSON or a self-contained HTML page (System > Export report, or Ctrl+K).";
        info->HasOptions = FALSE;
        info->Interface = &SrCommandsInterface;

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
            PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem),
            MenuItemCallback,
            NULL,
            &PluginMenuItemCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackMainMenuInitializing),
            MainMenuInitializingCallback,
            NULL,
            &MainMenuInitializingCallbackRegistration
            );

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
