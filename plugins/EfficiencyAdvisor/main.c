/*
 * TaskManagerPlus EfficiencyAdvisor plugin: registration, sampling callback and the palette command.
 */

#include "effadvisor.h"

extern VOID NTAPI EaProcessItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    ); // tracker.c

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PPH_MESSAGE_LOOP_FILTER_ENTRY FilterEntry = NULL;
static UINT CommandMessage = 0;

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI MessageLoopFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    if (CommandMessage && Message->message == CommandMessage)
    {
        switch (Message->wParam)
        {
        case 1:
            EaSelectTab();
            break;
        case 2:
        case 3:
        case 4:
            EaTestAction((ULONG)Message->wParam);
            break;
        case 5:
            EaRebuild(TRUE);
            break;
        }

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
    EaTrackerInitialize();
    CommandMessage = RegisterWindowMessage(COMMAND_MESSAGE_NAME);
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

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainWindowShowingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    EaInitializeTab();
}

/** Once per refresh, main thread. */
_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    EaSample();
    EaRebuild(FALSE);
}

// ---- palette command

static PPH_LIST NTAPI EaCommandsEnumerate(
    VOID
    )
{
    PPH_LIST commands = PhCreateList(1);
    PTMP_COMMAND_INFO command = PhAllocateZero(sizeof(TMP_COMMAND_INFO));

    command->Id = EA_COMMAND_SHOW;
    command->Label = PhCreateString(L"Show efficiency advisor");
    command->Detail = PhCreateString(L"Advisor");
    command->Alias = L"background offenders battery eco slow cpu memory";
    PhAddItemList(commands, command);
    return commands;
}

static VOID NTAPI EaCommandsFree(
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

static VOID NTAPI EaCommandsRun(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    )
{
    if (Id == EA_COMMAND_SHOW)
        EaSelectTab();
}

TMP_COMMANDS_INTERFACE EaCommandsInterface =
{
    TMP_COMMANDS_INTERFACE_VERSION,
    EaCommandsEnumerate,
    EaCommandsFree,
    EaCommandsRun
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
            { IntegerSettingType, SETTING_NAME_MIN_BACKGROUND_SECONDS, L"3c" }, // 60
            { IntegerSettingType, SETTING_NAME_HALF_LIFE_SECONDS, L"12c" }, // 300
            { IntegerSettingType, SETTING_NAME_CPU_THRESHOLD_TENTHS, L"a" }, // 1.0 %
            { IntegerSettingType, SETTING_NAME_MEMORY_THRESHOLD_MB, L"190" }, // 400
            { IntegerSettingType, SETTING_NAME_IO_THRESHOLD_KBPS, L"400" }, // 1024 KB/s
            { IntegerSettingType, SETTING_NAME_MIN_SCORE_TENTHS, L"a" }, // 1.0
            { StringSettingType, SETTING_NAME_IGNORED, L"" },
            { StringSettingType, SETTING_NAME_COLUMNS, L"" },
            { IntegerPairSettingType, SETTING_NAME_SORT, L"9,2" },
            { StringSettingType, SETTING_NAME_TEST_OUTPUT, L"" },
            { StringSettingType, SETTING_NAME_TEST_PROGRAM, L"" },
            { IntegerSettingType, SETTING_NAME_TEST_ALLOW_NO_FOREGROUND, L"0" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Efficiency Advisor";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Advisor tab: background programs that keep using CPU, memory or disk while you work in something else, with one-click Efficiency mode, lower priority and \"make a rule\". Never acts on its own.";
        info->HasOptions = FALSE;
        info->Interface = &EaCommandsInterface;

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
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
            ProcessesUpdatedCallback,
            NULL,
            &ProcessesUpdatedCallbackRegistration
            );

        PhPluginSetObjectExtension(
            PluginInstance,
            EmProcessItemType,
            sizeof(EA_EXTENSION),
            EaProcessItemCreateCallback,
            NULL
            );

        PhAddSettings(settings, RTL_NUMBER_OF(settings));
    }

    return TRUE;
}
