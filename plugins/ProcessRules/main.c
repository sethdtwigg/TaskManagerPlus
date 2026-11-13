/*
 * TaskManagerPlus ProcessRules plugin - registration
 */

#include "processrules.h"
#include <processrulesintf.h>

extern PROCESS_RULES_INTERFACE TmpInterface; // intf.c

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginShowOptionsCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessMenuInitializingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PPH_MESSAGE_LOOP_FILTER_ENTRY FilterEntry = NULL;
static UINT CommandMessage = 0;

// TaskManagerPlus: test seam for share codes (the clipboard cannot be driven on a hidden desktop). The registered message
// "TaskManagerPlus.ProcessRules.Command": wParam 1 writes the share code of all rules to the file in the TestShareFile setting,
// wParam 2 imports the code in that file and writes "OK <count>" or "FAIL <reason>" to <file>.result.
static VOID TmpTestWriteText(
    _In_ PPH_STRING FileName,
    _In_ PPH_STRING Text
    )
{
    HANDLE fileHandle;
    PPH_BYTES utf8 = PhConvertUtf16ToUtf8Ex(Text->Buffer, Text->Length);

    if (utf8 && NT_SUCCESS(PhCreateFileWin32(&fileHandle, FileName->Buffer, FILE_GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
    {
        PhWriteFile(fileHandle, utf8->Buffer, (ULONG)utf8->Length, NULL, NULL);
        NtClose(fileHandle);
    }

    if (utf8)
        PhDereferenceObject(utf8);
}

static PPH_STRING TmpTestReadText(
    _In_ PPH_STRING FileName
    )
{
    HANDLE fileHandle;
    LARGE_INTEGER size;
    PPH_STRING text = NULL;

    if (NT_SUCCESS(PhCreateFileWin32(&fileHandle, FileName->Buffer, FILE_GENERIC_READ, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
    {
        if (NT_SUCCESS(PhGetFileSize(fileHandle, &size)) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024)
        {
            PVOID buffer = PhAllocate((SIZE_T)size.QuadPart);
            ULONG read = 0;

            if (NT_SUCCESS(PhReadFile(fileHandle, buffer, (ULONG)size.QuadPart, NULL, &read)) && read)
                text = PhConvertUtf8ToUtf16Ex(buffer, read);

            PhFree(buffer);
        }

        NtClose(fileHandle);
    }

    return text;
}

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI MessageLoopFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    if (CommandMessage && Message->message == CommandMessage)
    {
        PPH_STRING file = PhGetStringSetting(SETTING_NAME_TEST_SHARE_FILE);

        if (file->Length)
        {
            if (Message->wParam == 1)
            {
                PPH_STRING code = TmpCreateShareCode(NULL, 0);

                if (code)
                {
                    TmpTestWriteText(file, code);
                    PhDereferenceObject(code);
                }
            }
            else if (Message->wParam == 2)
            {
                PPH_STRING text = TmpTestReadText(file);
                PPH_STRING resultFile = PhFormatString(L"%s.result", file->Buffer);
                PPH_STRING result;
                ULONG imported = 0;
                PCSTR reason = "";

                if (text && TmpImportShareCode(text, &imported, &reason))
                {
                    result = PhFormatString(L"OK %lu", imported);
                }
                else
                {
                    PPH_STRING why = PhConvertUtf8ToUtf16((PSTR)reason);

                    result = PhFormatString(L"FAIL %s", why ? why->Buffer : L"unreadable");
                    PhClearReference(&why);
                }

                TmpTestWriteText(resultFile, result);
                PhDereferenceObject(result);
                PhDereferenceObject(resultFile);
                PhClearReference(&text);
            }
        }

        PhDereferenceObject(file);
        return TRUE;
    }

    return FALSE;
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    TmpEngineInitialize();
    TmpLoadRules();
    CommandMessage = RegisterWindowMessage(COMMAND_MESSAGE_NAME);
    FilterEntry = PhRegisterMessageLoopFilter(MessageLoopFilter, NULL);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    if (FilterEntry)
    {
        PhUnregisterMessageLoopFilter(FilterEntry);
        FilterEntry = NULL;
    }

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
            { StringSettingType, SETTING_NAME_TEST_SHARE_FILE, L"" },
        };

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Process Rules";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Per-program rules: set priority, affinity (including P/E-core presets), efficiency mode, block or run a command when a program starts, and watchdogs that act when CPU, memory, I/O, handle or thread usage stays above a threshold.";
        info->HasOptions = TRUE;
        info->Interface = &TmpInterface; // TaskManagerPlus: rule states for Profiles and the Efficiency advisor

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
