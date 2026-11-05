/*
 * TaskManagerPlus ProcessRules plugin - rule edit dialog
 */

#include "processrules.h"

typedef struct _TMP_RULE_DIALOG_CONTEXT
{
    HWND WindowHandle;
    PTMP_RULE Rule;
} TMP_RULE_DIALOG_CONTEXT, *PTMP_RULE_DIALOG_CONTEXT;

static CONST ULONG TmpPriorityValues[] =
{
    0,
    PROCESS_PRIORITY_CLASS_IDLE,
    PROCESS_PRIORITY_CLASS_BELOW_NORMAL,
    PROCESS_PRIORITY_CLASS_NORMAL,
    PROCESS_PRIORITY_CLASS_ABOVE_NORMAL,
    PROCESS_PRIORITY_CLASS_HIGH,
    PROCESS_PRIORITY_CLASS_REALTIME
};

static CONST ULONG TmpWatchdogEnableIds[TMP_MAX_WATCHDOGS] = { IDC_WD_ENABLE0, IDC_WD_ENABLE1, IDC_WD_ENABLE2, IDC_WD_ENABLE3 };
static CONST ULONG TmpWatchdogMetricIds[TMP_MAX_WATCHDOGS] = { IDC_WD_METRIC0, IDC_WD_METRIC1, IDC_WD_METRIC2, IDC_WD_METRIC3 };
static CONST ULONG TmpWatchdogThresholdIds[TMP_MAX_WATCHDOGS] = { IDC_WD_THRESHOLD0, IDC_WD_THRESHOLD1, IDC_WD_THRESHOLD2, IDC_WD_THRESHOLD3 };
static CONST ULONG TmpWatchdogUnitIds[TMP_MAX_WATCHDOGS] = { IDC_WD_UNIT0, IDC_WD_UNIT1, IDC_WD_UNIT2, IDC_WD_UNIT3 };
static CONST ULONG TmpWatchdogDurationIds[TMP_MAX_WATCHDOGS] = { IDC_WD_DURATION0, IDC_WD_DURATION1, IDC_WD_DURATION2, IDC_WD_DURATION3 };
static CONST ULONG TmpWatchdogActionIds[TMP_MAX_WATCHDOGS] = { IDC_WD_ACTION0, IDC_WD_ACTION1, IDC_WD_ACTION2, IDC_WD_ACTION3 };

static BOOLEAN TmpMetricUsesMegabytes(
    _In_ TMP_METRIC Metric
    )
{
    return Metric == TmpMetricPrivateBytes || Metric == TmpMetricWorkingSet || Metric == TmpMetricIoRate;
}

static VOID TmpUpdateWatchdogRow(
    _In_ HWND WindowHandle,
    _In_ ULONG Index
    )
{
    BOOLEAN enabled;
    LONG metricIndex;

    enabled = Button_GetCheck(GetDlgItem(WindowHandle, TmpWatchdogEnableIds[Index])) == BST_CHECKED;
    metricIndex = ComboBox_GetCurSel(GetDlgItem(WindowHandle, TmpWatchdogMetricIds[Index]));

    if (metricIndex == CB_ERR)
        metricIndex = 0;

    PhSetDialogItemText(WindowHandle, TmpWatchdogUnitIds[Index], TmpGetMetricUnit((TMP_METRIC)metricIndex));

    EnableWindow(GetDlgItem(WindowHandle, TmpWatchdogMetricIds[Index]), enabled);
    EnableWindow(GetDlgItem(WindowHandle, TmpWatchdogThresholdIds[Index]), enabled);
    EnableWindow(GetDlgItem(WindowHandle, TmpWatchdogDurationIds[Index]), enabled);
    EnableWindow(GetDlgItem(WindowHandle, TmpWatchdogActionIds[Index]), enabled);
}

static VOID TmpLoadRuleIntoDialog(
    _In_ HWND WindowHandle,
    _In_ PTMP_RULE Rule
    )
{
    LONG index;

    PhSetDialogItemText(WindowHandle, IDC_PROGRAM, PhGetStringOrEmpty(Rule->ProgramName));
    PhSetDialogItemText(WindowHandle, IDC_MATCHPATH, PhGetStringOrEmpty(Rule->MatchPath));
    PhSetDialogItemText(WindowHandle, IDC_COMMAND, PhGetStringOrEmpty(Rule->Command));
    Button_SetCheck(GetDlgItem(WindowHandle, IDC_ENABLED), Rule->Enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(WindowHandle, IDC_ECO), Rule->StartEfficiencyMode ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(WindowHandle, IDC_ENDONSTART), Rule->StartEnd ? BST_CHECKED : BST_UNCHECKED);

    index = 0;
    for (ULONG i = 0; i < RTL_NUMBER_OF(TmpPriorityValues); i++)
    {
        if (TmpPriorityValues[i] == Rule->StartPriority)
            index = (LONG)i;
    }
    ComboBox_SetCurSel(GetDlgItem(WindowHandle, IDC_PRIORITY), index);

    index = (Rule->StartAffinity < TmpAffinityCustom) ? (LONG)Rule->StartAffinity : 0;
    ComboBox_SetCurSel(GetDlgItem(WindowHandle, IDC_AFFINITY), index);

    for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
    {
        PTMP_WATCHDOG watchdog = &Rule->Watchdogs[i];
        ULONG64 threshold = watchdog->Threshold;

        if (TmpMetricUsesMegabytes(watchdog->Metric))
            threshold /= (1024 * 1024);

        Button_SetCheck(GetDlgItem(WindowHandle, TmpWatchdogEnableIds[i]), watchdog->Enabled ? BST_CHECKED : BST_UNCHECKED);
        ComboBox_SetCurSel(GetDlgItem(WindowHandle, TmpWatchdogMetricIds[i]), (LONG)watchdog->Metric);
        PhSetDialogItemValue(WindowHandle, TmpWatchdogThresholdIds[i], (ULONG)threshold, FALSE);
        PhSetDialogItemValue(WindowHandle, TmpWatchdogDurationIds[i], watchdog->DurationSeconds, FALSE);
        ComboBox_SetCurSel(GetDlgItem(WindowHandle, TmpWatchdogActionIds[i]), (LONG)watchdog->Action);

        TmpUpdateWatchdogRow(WindowHandle, i);
    }
}

static BOOLEAN TmpSaveDialogIntoRule(
    _In_ HWND WindowHandle,
    _Inout_ PTMP_RULE Rule
    )
{
    PPH_STRING programName;
    PPH_STRING matchPath;
    PPH_STRING command;
    PH_STRINGREF baseName;
    PH_STRINGREF directory;
    LONG index;

    programName = PhGetWindowText(GetDlgItem(WindowHandle, IDC_PROGRAM));
    PhTrimToNullTerminatorString(programName);

    // Accept a full path and keep just the image name.
    if (PhSplitStringRefAtLastChar(&programName->sr, OBJ_NAME_PATH_SEPARATOR, &directory, &baseName))
        PhMoveReference(&programName, PhCreateString2(&baseName));

    if (PhIsNullOrEmptyString(programName))
    {
        PhShowError2(WindowHandle, L"Program name required", L"%s", L"Enter the image name of the program, for example notepad.exe.");
        PhDereferenceObject(programName);
        return FALSE;
    }

    matchPath = PhGetWindowText(GetDlgItem(WindowHandle, IDC_MATCHPATH));
    command = PhGetWindowText(GetDlgItem(WindowHandle, IDC_COMMAND));

    PhMoveReference(&Rule->ProgramName, programName);
    PhMoveReference(&Rule->MatchPath, matchPath);
    PhMoveReference(&Rule->Command, command);
    Rule->Enabled = Button_GetCheck(GetDlgItem(WindowHandle, IDC_ENABLED)) == BST_CHECKED;
    Rule->AutoDisabled = FALSE;
    Rule->StartEfficiencyMode = Button_GetCheck(GetDlgItem(WindowHandle, IDC_ECO)) == BST_CHECKED;
    Rule->StartEnd = Button_GetCheck(GetDlgItem(WindowHandle, IDC_ENDONSTART)) == BST_CHECKED;

    index = ComboBox_GetCurSel(GetDlgItem(WindowHandle, IDC_PRIORITY));
    Rule->StartPriority = (index >= 0 && index < (LONG)RTL_NUMBER_OF(TmpPriorityValues)) ? TmpPriorityValues[index] : 0;

    index = ComboBox_GetCurSel(GetDlgItem(WindowHandle, IDC_AFFINITY));
    Rule->StartAffinity = (index >= 0 && index < TmpAffinityCustom) ? (TMP_AFFINITY_PRESET)index : TmpAffinityNone;

    for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
    {
        PTMP_WATCHDOG watchdog = &Rule->Watchdogs[i];
        ULONG64 threshold;

        watchdog->Enabled = Button_GetCheck(GetDlgItem(WindowHandle, TmpWatchdogEnableIds[i])) == BST_CHECKED;

        index = ComboBox_GetCurSel(GetDlgItem(WindowHandle, TmpWatchdogMetricIds[i]));
        watchdog->Metric = (index >= 0 && index < TmpMetricMaximum) ? (TMP_METRIC)index : TmpMetricCpu;

        index = ComboBox_GetCurSel(GetDlgItem(WindowHandle, TmpWatchdogActionIds[i]));
        watchdog->Action = (index >= 0 && index < TmpActionMaximum) ? (TMP_ACTION)index : TmpActionNotify;

        threshold = PhGetDialogItemValue(WindowHandle, TmpWatchdogThresholdIds[i]);

        if (TmpMetricUsesMegabytes(watchdog->Metric))
            threshold *= (1024 * 1024);

        watchdog->Threshold = threshold;
        watchdog->DurationSeconds = (ULONG)PhGetDialogItemValue(WindowHandle, TmpWatchdogDurationIds[i]);
    }

    TmpInvalidateRuleDisplay(Rule);

    return TRUE;
}

static VOID TmpBrowseForProgram(
    _In_ HWND WindowHandle
    )
{
    static CONST PH_FILETYPE_FILTER filters[] =
    {
        { L"Executable files (*.exe)", L"*.exe" },
        { L"All files (*.*)", L"*.*" }
    };
    PVOID fileDialog;
    PPH_STRING fileName;
    PPH_STRING baseName;

    fileDialog = PhCreateOpenFileDialog();
    PhSetFileDialogFilter(fileDialog, (PPH_FILETYPE_FILTER)filters, RTL_NUMBER_OF(filters));

    if (PhShowFileDialog(WindowHandle, fileDialog))
    {
        fileName = PhGetFileDialogFileName(fileDialog);
        baseName = PhGetBaseName(fileName);

        PhSetDialogItemText(WindowHandle, IDC_PROGRAM, PhGetString(baseName));
        PhSetDialogItemText(WindowHandle, IDC_MATCHPATH, PhGetString(fileName));

        PhDereferenceObject(baseName);
        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(fileDialog);
}

static INT_PTR CALLBACK TmpRuleDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PTMP_RULE_DIALOG_CONTEXT context;

    if (WindowMessage == WM_INITDIALOG)
    {
        context = (PTMP_RULE_DIALOG_CONTEXT)lParam;
        PhSetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT, context);
    }
    else
    {
        context = PhGetWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
    }

    if (!context)
        return FALSE;

    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            static CONST PCWSTR priorityNames[] = { L"No change", L"Idle", L"Below normal", L"Normal", L"Above normal", L"High", L"Real time" };
            static CONST PCWSTR affinityNames[] = { L"No change", L"All processors", L"P-cores only (performance)", L"E-cores only (efficient)" };
            static CONST PCWSTR metricNames[] = { L"CPU usage", L"Private bytes", L"Working set", L"I/O rate", L"Handles", L"Threads" };
            static CONST PCWSTR actionNames[] = { L"Notify", L"Log only", L"Lower priority", L"Efficiency mode", L"Suspend", L"End process", L"Write dump", L"Run command" };

            context->WindowHandle = WindowHandle;

            PhCenterWindow(WindowHandle, GetParent(WindowHandle));
            PhSetApplicationWindowIcon(WindowHandle);

            PhAddComboBoxStrings(GetDlgItem(WindowHandle, IDC_PRIORITY), (PCWSTR*)priorityNames, RTL_NUMBER_OF(priorityNames));
            PhAddComboBoxStrings(GetDlgItem(WindowHandle, IDC_AFFINITY), (PCWSTR*)affinityNames, RTL_NUMBER_OF(affinityNames));

            for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
            {
                PhAddComboBoxStrings(GetDlgItem(WindowHandle, TmpWatchdogMetricIds[i]), (PCWSTR*)metricNames, RTL_NUMBER_OF(metricNames));
                PhAddComboBoxStrings(GetDlgItem(WindowHandle, TmpWatchdogActionIds[i]), (PCWSTR*)actionNames, RTL_NUMBER_OF(actionNames));
            }

            if (!TmpIsHybridSystem())
            {
                PhSetDialogItemText(WindowHandle, IDC_HYBRIDNOTE,
                    L"Command tokens: {pid} {name} {path}. This CPU has no P/E cores; those presets leave affinity unchanged.");
            }

            TmpLoadRuleIntoDialog(WindowHandle, context->Rule);

            PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(L"EnableThemeSupport"));
            PhSetDialogFocus(WindowHandle, GetDlgItem(WindowHandle, IDC_PROGRAM));
        }
        break;
    case WM_DESTROY:
        {
            PhRemoveWindowContext(WindowHandle, PH_WINDOW_CONTEXT_DEFAULT);
        }
        break;
    case WM_COMMAND:
        {
            ULONG id = GET_WM_COMMAND_ID(wParam, lParam);
            ULONG code = GET_WM_COMMAND_CMD(wParam, lParam);

            switch (id)
            {
            case IDCANCEL:
                EndDialog(WindowHandle, IDCANCEL);
                break;
            case IDOK:
                {
                    if (TmpSaveDialogIntoRule(WindowHandle, context->Rule))
                        EndDialog(WindowHandle, IDOK);
                }
                break;
            case IDC_BROWSE:
                TmpBrowseForProgram(WindowHandle);
                break;
            default:
                {
                    for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
                    {
                        if ((id == TmpWatchdogEnableIds[i] && code == BN_CLICKED) ||
                            (id == TmpWatchdogMetricIds[i] && code == CBN_SELCHANGE))
                        {
                            TmpUpdateWatchdogRow(WindowHandle, i);
                        }
                    }
                }
                break;
            }
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

/**
 * Shows the rule editor. The rule is modified in place only when OK is pressed.
 */
BOOLEAN TmpShowRuleDialog(
    _In_ HWND ParentWindowHandle,
    _Inout_ PTMP_RULE Rule
    )
{
    TMP_RULE_DIALOG_CONTEXT context;
    PTMP_RULE workingCopy;
    INT_PTR result;

    // Edit a copy so that Cancel leaves the rule untouched.
    workingCopy = TmpCreateRule();
    TmpCopyRule(workingCopy, Rule);

    memset(&context, 0, sizeof(context));
    context.Rule = workingCopy;

    result = PhDialogBox(
        PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_RULE),
        ParentWindowHandle,
        TmpRuleDlgProc,
        &context
        );

    if (result == IDOK)
        TmpCopyRule(Rule, workingCopy);

    TmpDeleteRule(workingCopy);

    return result == IDOK;
}
