/*
 * TaskManagerPlus Profiles plugin: registration, applying and reverting profiles, the System > Profiles menu and the
 * palette commands.
 */

#include "profiles.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainMenuInitializingCallbackRegistration;
static PPH_MESSAGE_LOOP_FILTER_ENTRY FilterEntry = NULL;
static UINT CommandMessage = 0;

#define PR_MENU_SAVE 1
#define PR_MENU_MANAGE 2

static PPROCESS_RULES_INTERFACE PrGetRules(
    VOID
    )
{
    return PhGetPluginInterfaceZ(PROCESS_RULES_PLUGIN_NAME, PROCESS_RULES_INTERFACE_VERSION);
}

/** The current rules as a malloc'ed array, or NULL (with *Count 0) when the Process Rules plugin is not loaded. */
static PF_CURRENT_RULE *PrGatherCurrent(
    _Out_ PULONG Count
    )
{
    PPROCESS_RULES_INTERFACE rules = PrGetRules();
    PPH_LIST list;
    PF_CURRENT_RULE *current;
    ULONG i;

    *Count = 0;

    if (!rules)
        return NULL;

    list = rules->Enumerate();
    current = PhAllocateZero(sizeof(PF_CURRENT_RULE) * (list->Count + 1));

    for (i = 0; i < list->Count; i++)
    {
        PPROCESS_RULE_INFO info = list->Items[i];

        wcsncpy_s(current[i].Program, PF_PROGRAM_CHARS, PhGetStringOrEmpty(info->ProgramName), _TRUNCATE);
        current[i].Enabled = info->Enabled;
        current[i].Priority = info->StartPriority;
    }

    *Count = list->Count;
    rules->FreeRules(list);
    return current;
}

static INT PrGetTheme(
    VOID
    )
{
    return (INT)PhGetIntegerSetting(L"ThemeMode");
}

static VOID PrCarryOutPlan(
    _In_ PF_PLAN *Plan
    )
{
    PPROCESS_RULES_INTERFACE rules = PrGetRules();
    ULONG i;

    if (rules && Plan->ChangeCount)
    {
        for (i = 0; i < Plan->ChangeCount; i++)
        {
            PPH_STRING program = PhCreateString((PWSTR)Plan->Changes[i].Program);
            ULONG mode = Plan->Changes[i].EnabledMode == PF_ON ? PROCESS_RULES_SET_ON : Plan->Changes[i].EnabledMode == PF_OFF ? PROCESS_RULES_SET_OFF : PROCESS_RULES_KEEP;

            rules->SetRuleState(program, mode, Plan->Changes[i].HasPriority, Plan->Changes[i].Priority);
            PhDereferenceObject(program);
        }

        rules->Commit();
    }

    if (Plan->ThemeChange)
        PhApplyThemeModeProfile(Plan->NewTheme);
}

static VOID PrAnnounce(
    _In_ PCWSTR Title,
    _In_ PF_PLAN *Plan
    )
{
    PPH_STRING text = PhFormatString(
        L"%s: %lu rule change(s)%s%s",
        Title,
        Plan->ChangeCount,
        Plan->ThemeChange ? L", theme changed" : L"",
        Plan->MissingRules ? L" (some rules named in the profile do not exist here)" : L""
        );

    PhShowIconNotification(L"TaskManagerPlus profiles", text->Buffer);
    PhLogMessageEntry(PH_LOG_ENTRY_MESSAGE, text); // consumes text
}

BOOLEAN PrSaveCurrentAs(
    _In_ PCWSTR Name,
    _In_ unsigned CaptureFlags
    )
{
    PF_PROFILE *profile = PfAllocProfile();
    PF_CURRENT_RULE *current;
    ULONG count;
    BOOLEAN ok = FALSE;

    if (!profile)
        return FALSE;

    current = PrGatherCurrent(&count);

    if (PfCapture(profile, Name, current, count, PrGetTheme(), CaptureFlags) == 0 && PfStoreSet(&PrStore, profile) == 0)
    {
        wcsncpy_s(PrStore.Active, PF_NAME_CHARS, profile->Name, _TRUNCATE);
        PrSave();
        ok = TRUE;
    }

    if (current)
        PhFree(current);

    PfFreeProfile(profile);
    return ok;
}

BOOLEAN PrApplyProfile(
    _In_ PCWSTR Name
    )
{
    PF_PLAN *plan = PhAllocateZero(sizeof(PF_PLAN));
    PF_CURRENT_RULE *current;
    ULONG count;
    BOOLEAN ok;

    current = PrGatherCurrent(&count);
    ok = PfStoreApply(&PrStore, Name, current, count, PrGetTheme(), plan) == 0;

    if (ok)
    {
        PrCarryOutPlan(plan);
        PrSave();
        PrAnnounce(PrStore.Active, plan);
    }

    if (current)
        PhFree(current);

    PhFree(plan);
    return ok;
}

BOOLEAN PrRevert(
    VOID
    )
{
    PF_PLAN *plan = PhAllocateZero(sizeof(PF_PLAN));
    PF_CURRENT_RULE *current;
    PF_PROFILE *source = NULL;
    ULONG count;
    BOOLEAN ok;

    current = PrGatherCurrent(&count);
    ok = PfStoreRevert(&PrStore, current, count, PrGetTheme(), plan, &source) == 0;

    if (ok)
    {
        PrCarryOutPlan(plan);
        PrSave();
        PrAnnounce(L"Reverted to the previous settings", plan);
    }

    PfFreeProfile(source);

    if (current)
        PhFree(current);

    PhFree(plan);
    return ok;
}

// ---- test seam: registered message, wParam = op

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI MessageLoopFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    if (CommandMessage && Message->message == CommandMessage)
    {
        PPH_STRING name = PhGetStringSetting(SETTING_NAME_TEST_NAME);
        PPH_STRING file = PhGetStringSetting(SETTING_NAME_TEST_FILE);
        ULONG imported;
        ULONG skipped;

        switch (Message->wParam)
        {
        case 1: // save the current state as profile TestName
            PrSaveCurrentAs(name->Buffer, PF_CAPTURE_ALL);
            break;
        case 2: // apply TestName
            PrApplyProfile(name->Buffer);
            break;
        case 3: // revert
            PrRevert();
            break;
        case 4: // export all to TestFile
            PrExportToFile(file, NULL);
            break;
        case 5: // import TestFile
            PrImportFromFile(file, &imported, &skipped);
            break;
        case 6:
            PrShowManageDialog(SystemInformer_GetWindowHandle());
            break;
        case 7:
            PrShowSaveDialog(SystemInformer_GetWindowHandle());
            break;
        }

        PhDereferenceObject(name);
        PhDereferenceObject(file);
        return TRUE;
    }

    return FALSE;
}

// ---- menu and commands

static VOID PrRunCommand(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    )
{
    if (Id == PR_COMMAND_REVERT)
    {
        if (!PrRevert())
            PhShowInformation2(OwnerWindow, L"Profiles", L"%s", L"There is nothing to revert: no profile has been applied since the settings were last saved or reverted.");
    }
    else if (Id >= PR_COMMAND_APPLY_BASE && Id - PR_COMMAND_APPLY_BASE < PrStore.Count)
    {
        PWSTR name = PrStore.Profiles[Id - PR_COMMAND_APPLY_BASE]->Name;
        PPH_STRING copy = PhCreateString(name);

        PrApplyProfile(copy->Buffer);
        PhDereferenceObject(copy);
    }
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;

    if (!menuItem)
        return;

    if (menuItem->Id == PR_MENU_SAVE)
        PrShowSaveDialog(menuItem->OwnerWindow);
    else if (menuItem->Id == PR_MENU_MANAGE)
        PrShowManageDialog(menuItem->OwnerWindow);
    else
        PrRunCommand(menuItem->Id, menuItem->OwnerWindow);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_EMENU_ITEM menu;
    PPH_EMENU_ITEM revert;
    ULONG i;

    if (!menuInfo || menuInfo->u.MainMenu.SubMenuIndex != PH_MENU_ITEM_LOCATION_SYSTEM)
        return;

    menu = PhPluginCreateEMenuItem(PluginInstance, 0, 0, L"Pro&files", NULL);

    for (i = 0; i < PrStore.Count; i++)
    {
        PPH_EMENU_ITEM item = PhPluginCreateEMenuItem(PluginInstance, 0, PR_COMMAND_APPLY_BASE + i, PrStore.Profiles[i]->Name, NULL);

        if (!_wcsicmp(PrStore.Active, PrStore.Profiles[i]->Name))
            item->Flags |= PH_EMENU_CHECKED;

        PhInsertEMenuItem(menu, item, ULONG_MAX);
    }

    if (PrStore.Count)
        PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);

    PhInsertEMenuItem(menu, revert = PhPluginCreateEMenuItem(PluginInstance, 0, PR_COMMAND_REVERT, L"&Revert to previous settings", NULL), ULONG_MAX);

    if (!PrStore.Previous)
        revert->Flags |= PH_EMENU_DISABLED;

    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhPluginCreateEMenuItem(PluginInstance, 0, PR_MENU_SAVE, L"&Save current settings as profile...", NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhPluginCreateEMenuItem(PluginInstance, 0, PR_MENU_MANAGE, L"&Manage profiles...", NULL), ULONG_MAX);
    PhInsertEMenuItem(menuInfo->Menu, menu, 0);
}

static PPH_LIST NTAPI PrCommandsEnumerate(
    VOID
    )
{
    PPH_LIST commands = PhCreateList(PF_MAX_PROFILES + 2);
    PTMP_COMMAND_INFO command;
    ULONG i;

    for (i = 0; i < PrStore.Count; i++)
    {
        command = PhAllocateZero(sizeof(TMP_COMMAND_INFO));
        command->Id = PR_COMMAND_APPLY_BASE + i;
        command->Label = PhFormatString(L"Switch to profile %s", PrStore.Profiles[i]->Name);
        command->Detail = PhCreateString(!_wcsicmp(PrStore.Active, PrStore.Profiles[i]->Name) ? L"Profile (active)" : L"Profile");
        command->Alias = L"profile preset mode";
        PhAddItemList(commands, command);
    }

    if (PrStore.Previous)
    {
        command = PhAllocateZero(sizeof(TMP_COMMAND_INFO));
        command->Id = PR_COMMAND_REVERT;
        command->Label = PhCreateString(L"Revert to previous profile settings");
        command->Detail = PhCreateString(L"Profile");
        command->Alias = L"undo profile";
        PhAddItemList(commands, command);
    }

    return commands;
}

static VOID NTAPI PrCommandsFree(
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

static VOID NTAPI PrCommandsRun(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    )
{
    PrRunCommand(Id, OwnerWindow);
}

TMP_COMMANDS_INTERFACE PrCommandsInterface =
{
    TMP_COMMANDS_INTERFACE_VERSION,
    PrCommandsEnumerate,
    PrCommandsFree,
    PrCommandsRun
};

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PrLoad();
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

    PfStoreFree(&PrStore);
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
            { StringSettingType, SETTING_NAME_TEST_NAME, L"" },
            { StringSettingType, SETTING_NAME_TEST_FILE, L"" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Profiles";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Named setting profiles (Work, Gaming ...): which Process Rules are on, their start priority and the theme. Switch from System > Profiles or Ctrl+K, revert to the previous settings, import and export as JSON.";
        info->HasOptions = FALSE;
        info->Interface = &PrCommandsInterface;

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
