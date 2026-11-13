/*
 * TaskManagerPlus EfficiencyAdvisor: the Advisor tab. A list of background offenders, four buttons (Efficiency mode, Lower
 * priority, Make a rule, Ignore) and an info line. Every action is a click on the selected row; nothing is automatic.
 */

#include "effadvisor.h"

#define EA_TAB_CLASS L"TaskManagerPlus.EfficiencyAdvisor.Tab"

enum
{
    EA_COL_NAME, EA_COL_PID, EA_COL_WHY, EA_COL_CPU, EA_COL_MEMORY, EA_COL_DISK, EA_COL_BACKGROUND, EA_COL_EFFICIENCY,
    EA_COL_PRIORITY, EA_COL_SCORE, EA_COL_MAXIMUM
};

enum
{
    EA_ID_EFFICIENCY = 5001, EA_ID_LOWER, EA_ID_RULE, EA_ID_IGNORE, EA_ID_REFRESH, EA_ID_INFO,
    EA_MENU_GOTO = 5101, EA_MENU_COPY
};

static CONST PH_STRINGREF EaPageName = PH_STRINGREF_INIT(L"Advisor");
static CONST PH_STRINGREF EaEmptyText = PH_STRINGREF_INIT(L"Nothing to suggest right now: no background program has used a lot of CPU, memory or disk for a while.");

static PPH_MAIN_TAB_PAGE EaTabPage = NULL;
static HWND EaTabWindow = NULL;
static HWND EaTree = NULL;
static HWND EaButtons[4];
static HWND EaRefreshButton = NULL;
static HWND EaInfo = NULL;
static HFONT EaFont = NULL;
static BOOLEAN EaCreated = FALSE;
static BOOLEAN EaSelected = FALSE;
static PPH_LIST EaRows = NULL;
static ULONG64 EaLastRebuild = 0;
static ULONG EaSortColumn = EA_COL_SCORE;
static PH_SORT_ORDER EaSortOrder = DescendingSortOrder;

static VOID EaLayout(
    VOID
    );

static VOID EaUpdateButtons(
    VOID
    );

static PEA_ROW EaGetSelectedRow(
    VOID
    )
{
    ULONG i;

    if (!EaRows)
        return NULL;

    for (i = 0; i < EaRows->Count; i++)
    {
        PEA_ROW row = EaRows->Items[i];

        if (row->Node.Selected)
            return row;
    }

    return NULL;
}

/** The live process item behind the selected row (a pid can be reused: the start time must match). Caller dereferences. */
static PPH_PROCESS_ITEM EaReferenceRowProcess(
    _In_ PEA_ROW Row,
    _In_ BOOLEAN Complain
    )
{
    PPH_PROCESS_ITEM process = PhReferenceProcessItem(UlongToHandle(Row->ProcessId));

    if (process && process->CreateTime.QuadPart != Row->CreateTime)
    {
        PhDereferenceObject(process);
        process = NULL;
    }

    if (!process && Complain)
        PhShowInformation2(SystemInformer_GetWindowHandle(), L"Efficiency advisor", L"%s (PID %lu) is no longer running.", PhGetStringOrDefault(Row->Name, L"The process"), Row->ProcessId);

    return process;
}

// ---------------------------------------------------------------- actions (always on a click)

static VOID EaDoEfficiency(
    _In_ HWND Owner,
    _In_ PEA_ROW Row
    )
{
    PPH_PROCESS_ITEM process = EaReferenceRowProcess(Row, TRUE);

    if (process)
    {
        PhUiSetEcoModeProcess(Owner, process);
        PhDereferenceObject(process);
        EaLastRebuild = 0;
    }
}

static VOID EaDoLowerPriority(
    _In_ HWND Owner,
    _In_ PEA_ROW Row
    )
{
    PPH_PROCESS_ITEM process = EaReferenceRowProcess(Row, TRUE);

    if (process)
    {
        PhUiSetPriorityClassProcesses(Owner, &process, 1, PROCESS_PRIORITY_CLASS_BELOW_NORMAL);
        PhDereferenceObject(process);
        EaLastRebuild = 0;
    }
}

static VOID EaDoRule(
    _In_ HWND Owner,
    _In_ PEA_ROW Row
    )
{
    PPROCESS_RULES_INTERFACE rules = PhGetPluginInterfaceZ(PROCESS_RULES_PLUGIN_NAME, PROCESS_RULES_INTERFACE_VERSION);
    PPH_PROCESS_ITEM process;

    if (!rules)
    {
        PhShowInformation2(Owner, L"Efficiency advisor", L"%s", L"The Process Rules plugin is not loaded.");
        return;
    }

    process = EaReferenceRowProcess(Row, TRUE);

    if (process)
    {
        rules->EditRuleForProcess(Owner, process, PROCESS_PRIORITY_CLASS_BELOW_NORMAL, TRUE);
        PhDereferenceObject(process);
    }
}

static VOID EaDoIgnore(
    _In_ PEA_ROW Row
    )
{
    if (Row->Name)
    {
        EaAddIgnored(Row->Name);
        EaLastRebuild = 0;
        EaRebuild(TRUE);
    }
}

static VOID EaDoGoTo(
    _In_ PEA_ROW Row
    )
{
    PPH_PROCESS_NODE node = PhFindProcessNode(UlongToHandle(Row->ProcessId));

    if (node)
    {
        SystemInformer_SelectTabPage(0);
        SystemInformer_SelectProcessNode(node);
    }
}

static VOID EaDoCopy(
    VOID
    )
{
    PPH_STRING text = PhGetTreeNewText(EaTree, 0);

    PhSetClipboardString(EaTree, &text->sr);
    PhDereferenceObject(text);
}

// ---------------------------------------------------------------- tree

static int __cdecl EaRowCompare(
    _In_ const void *A,
    _In_ const void *B
    )
{
    PEA_ROW a = *(PEA_ROW *)A;
    PEA_ROW b = *(PEA_ROW *)B;
    int result = 0;

    switch (EaSortColumn)
    {
    case EA_COL_NAME:
        result = PhCompareStringWithNull(a->Name, b->Name, TRUE);
        break;
    case EA_COL_PID:
        result = a->ProcessId < b->ProcessId ? -1 : a->ProcessId > b->ProcessId;
        break;
    case EA_COL_WHY:
        result = (int)a->Reason - (int)b->Reason;
        break;
    case EA_COL_CPU:
        result = a->Stats.AvgCpuPercent < b->Stats.AvgCpuPercent ? -1 : a->Stats.AvgCpuPercent > b->Stats.AvgCpuPercent;
        break;
    case EA_COL_MEMORY:
        result = a->Stats.WorkingSetKB < b->Stats.WorkingSetKB ? -1 : a->Stats.WorkingSetKB > b->Stats.WorkingSetKB;
        break;
    case EA_COL_DISK:
        result = a->Stats.AvgIoBytesPerSecond < b->Stats.AvgIoBytesPerSecond ? -1 : a->Stats.AvgIoBytesPerSecond > b->Stats.AvgIoBytesPerSecond;
        break;
    case EA_COL_BACKGROUND:
        result = a->Stats.BackgroundSeconds < b->Stats.BackgroundSeconds ? -1 : a->Stats.BackgroundSeconds > b->Stats.BackgroundSeconds;
        break;
    case EA_COL_EFFICIENCY:
        result = (int)a->EfficiencyMode - (int)b->EfficiencyMode;
        break;
    case EA_COL_PRIORITY:
        result = (int)a->PriorityClass - (int)b->PriorityClass;
        break;
    default:
        result = a->Score < b->Score ? -1 : a->Score > b->Score;
        break;
    }

    if (!result)
        result = a->ProcessId < b->ProcessId ? -1 : a->ProcessId > b->ProcessId;

    return PhModifySort(result, EaSortOrder);
}

static BOOLEAN NTAPI EaTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_opt_ PVOID Context
    )
{
    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;

            if (getChildren->Node)
                break;

            if (EaRows && EaSortOrder != NoSortOrder && EaRows->Count > 1)
                qsort(EaRows->Items, EaRows->Count, sizeof(PVOID), EaRowCompare);

            getChildren->Children = EaRows ? (PPH_TREENEW_NODE *)EaRows->Items : NULL;
            getChildren->NumberOfChildren = EaRows ? EaRows->Count : 0;
        }
        return TRUE;
    case TreeNewIsLeaf:
        {
            PPH_TREENEW_IS_LEAF isLeaf = Parameter1;

            isLeaf->IsLeaf = TRUE;
        }
        return TRUE;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;
            PEA_ROW row = (PEA_ROW)getCellText->Node;

            switch (getCellText->Id)
            {
            case EA_COL_NAME: getCellText->Text = PhGetStringRef(row->Name); break;
            case EA_COL_PID: getCellText->Text = PhGetStringRef(row->PidText); break;
            case EA_COL_WHY: getCellText->Text = PhGetStringRef(row->Why); break;
            case EA_COL_CPU: getCellText->Text = PhGetStringRef(row->Cpu); break;
            case EA_COL_MEMORY: getCellText->Text = PhGetStringRef(row->Memory); break;
            case EA_COL_DISK: getCellText->Text = PhGetStringRef(row->Disk); break;
            case EA_COL_BACKGROUND: getCellText->Text = PhGetStringRef(row->Background); break;
            case EA_COL_EFFICIENCY: getCellText->Text = PhGetStringRef(row->Efficiency); break;
            case EA_COL_PRIORITY: getCellText->Text = PhGetStringRef(row->Priority); break;
            case EA_COL_SCORE: getCellText->Text = PhGetStringRef(row->ScoreText); break;
            default: return FALSE;
            }

            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewSortChanged:
        {
            PPH_TREENEW_SORT_CHANGED_EVENT sorting = Parameter1;

            EaSortColumn = sorting->SortColumn;
            EaSortOrder = sorting->SortOrder;
            TreeNew_NodesStructured(WindowHandle);
        }
        return TRUE;
    case TreeNewSelectionChanged:
        EaUpdateButtons();
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            if (keyEvent->VirtualKey == 'C' && GetKeyState(VK_CONTROL) < 0)
                EaDoCopy();
        }
        return TRUE;
    case TreeNewLeftDoubleClick:
        {
            PEA_ROW row = EaGetSelectedRow();

            if (row)
                EaDoGoTo(row);
        }
        return TRUE;
    case TreeNewContextMenu:
        {
            PPH_TREENEW_CONTEXT_MENU contextMenuEvent = Parameter1;
            PEA_ROW row = EaGetSelectedRow();
            PPH_EMENU menu = PhCreateEMenu();
            PPH_EMENU_ITEM selected;

            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, EA_ID_EFFICIENCY, L"&Efficiency mode", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, EA_ID_LOWER, L"&Lower priority (below normal)", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, EA_ID_RULE, L"Make a &rule...", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, EA_ID_IGNORE, L"&Ignore this program", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, EA_MENU_GOTO, L"&Go to process", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, EA_MENU_COPY, L"&Copy\bCtrl+C", NULL, NULL), ULONG_MAX);

            if (!row)
            {
                PhSetFlagsAllEMenuItems(menu, PH_EMENU_DISABLED, PH_EMENU_DISABLED);
            }

            selected = PhShowEMenu(menu, WindowHandle, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, contextMenuEvent->Location.x, contextMenuEvent->Location.y);

            if (selected && row)
            {
                HWND owner = SystemInformer_GetWindowHandle();

                switch (selected->Id)
                {
                case EA_ID_EFFICIENCY: EaDoEfficiency(owner, row); break;
                case EA_ID_LOWER: EaDoLowerPriority(owner, row); break;
                case EA_ID_RULE: EaDoRule(owner, row); break;
                case EA_ID_IGNORE: EaDoIgnore(row); break;
                case EA_MENU_GOTO: EaDoGoTo(row); break;
                case EA_MENU_COPY: EaDoCopy(); break;
                }
            }

            PhDestroyEMenu(menu);
        }
        return TRUE;
    case TreeNewHeaderRightClick:
        {
            PH_TN_COLUMN_MENU_DATA data;

            data.TreeNewHandle = WindowHandle;
            data.MouseEvent = Parameter1;
            data.DefaultSortColumn = EA_COL_SCORE;
            data.DefaultSortOrder = DescendingSortOrder;
            PhInitializeTreeNewColumnMenuEx(&data, PH_TN_COLUMN_MENU_SHOW_RESET_SORT);
            data.Selection = PhShowEMenu(data.Menu, WindowHandle, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, data.MouseEvent->ScreenLocation.x, data.MouseEvent->ScreenLocation.y);
            PhHandleTreeNewColumnMenu(&data);
            PhDeleteTreeNewColumnMenu(&data);
        }
        return TRUE;
    }

    return FALSE;
}

static VOID EaUpdateButtons(
    VOID
    )
{
    BOOLEAN has = EaGetSelectedRow() != NULL;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(EaButtons); i++)
        EnableWindow(EaButtons[i], has);
}

static VOID EaUpdateInfo(
    VOID
    )
{
    PPH_STRING text;
    AD_CONFIG config;

    EaGetConfig(&config);
    text = PhFormatString(
        L"%lu background program(s) are being watched. Listed: in the background for at least %.0f s and using about %.1f %% of the CPU, %.0f MB of memory or %.1f MB/s of disk. Nothing is changed unless you click a button. System, protected and service processes are never listed.",
        EaGetWatchedCount(), config.MinBackgroundSeconds, config.CpuThresholdPercent, config.MemoryThresholdMB, config.IoThresholdMBps
        );
    SetWindowText(EaInfo, text->Buffer);
    PhDereferenceObject(text);
}

VOID EaRebuild(
    _In_ BOOLEAN Force
    )
{
    ULONG64 now = NtGetTickCount64();
    PPH_LIST rows;
    ULONG selectedPid = 0;
    LONGLONG selectedTime = 0;
    PPH_LIST old;
    PPH_STRING testOutput;
    BOOLEAN needed;

    if (!Force && EaLastRebuild && now - EaLastRebuild < EA_REBUILD_INTERVAL_MS)
        return;

    testOutput = PhGetStringSetting(SETTING_NAME_TEST_OUTPUT);
    needed = (EaCreated && EaSelected) || testOutput->Length;
    PhDereferenceObject(testOutput);

    if (!needed && !Force)
        return;

    EaLastRebuild = now;
    rows = EaBuildRows();

    if (!EaCreated)
    {
        EaFreeRows(rows);
        return;
    }

    {
        PEA_ROW selected = EaGetSelectedRow();

        if (selected)
        {
            selectedPid = selected->ProcessId;
            selectedTime = selected->CreateTime;
        }
    }

    TreeNew_SetRedraw(EaTree, FALSE);
    old = EaRows;
    EaRows = rows;
    TreeNew_NodesStructured(EaTree);

    if (selectedPid)
    {
        ULONG i;

        for (i = 0; i < rows->Count; i++)
        {
            PEA_ROW row = rows->Items[i];

            if (row->ProcessId == selectedPid && row->CreateTime == selectedTime)
            {
                row->Node.Selected = TRUE;
                break;
            }
        }
    }

    TreeNew_SetRedraw(EaTree, TRUE);
    InvalidateRect(EaTree, NULL, FALSE);

    if (old)
        EaFreeRows(old);

    EaUpdateButtons();
    EaUpdateInfo();
}

// ---------------------------------------------------------------- window

static VOID EaLayout(
    VOID
    )
{
    RECT client;
    LONG dpi;
    LONG pad;
    LONG buttonHeight;
    LONG infoHeight;
    LONG x;
    LONG widths[4] = { 130, 150, 120, 100 };
    HDWP defer;
    ULONG i;

    if (!EaCreated)
        return;

    PhGetClientRect(EaTabWindow, &client);
    dpi = PhGetWindowDpi(EaTabWindow);
    pad = PhScaleToDisplay(8, dpi);
    buttonHeight = PhScaleToDisplay(26, dpi);
    infoHeight = PhScaleToDisplay(44, dpi);
    defer = BeginDeferWindowPos(8);
    x = pad;

    for (i = 0; i < RTL_NUMBER_OF(EaButtons); i++)
    {
        LONG width = PhScaleToDisplay(widths[i], dpi);

        defer = DeferWindowPos(defer, EaButtons[i], NULL, x, pad, width, buttonHeight, SWP_NOZORDER);
        x += width + pad;
    }

    defer = DeferWindowPos(defer, EaRefreshButton, NULL, x, pad, PhScaleToDisplay(90, dpi), buttonHeight, SWP_NOZORDER);
    defer = DeferWindowPos(defer, EaTree, NULL, 0, pad * 2 + buttonHeight, client.right, max(0, client.bottom - (pad * 2 + buttonHeight) - infoHeight - pad), SWP_NOZORDER);
    defer = DeferWindowPos(defer, EaInfo, NULL, pad, client.bottom - infoHeight - pad / 2, max(0, client.right - pad * 2), infoHeight, SWP_NOZORDER);
    EndDeferWindowPos(defer);
}

static VOID EaUpdateFont(
    VOID
    )
{
    HFONT old = EaFont;
    HWND controls[] = { EaButtons[0], EaButtons[1], EaButtons[2], EaButtons[3], EaRefreshButton, EaInfo };
    ULONG i;

    EaFont = PhCreateApplicationFont(PhGetWindowDpi(EaTabWindow));

    if (EaFont)
    {
        for (i = 0; i < RTL_NUMBER_OF(controls); i++)
            SetWindowFont(controls[i], EaFont, TRUE);
    }

    if (old)
        DeleteFont(old);
}

static LRESULT CALLBACK EaTabWndProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_SIZE:
        EaLayout();
        break;
    case WM_ERASEBKGND:
        {
            RECT rect;

            GetClientRect(WindowHandle, &rect);
            FillRect((HDC)wParam, &rect, PhGetIntegerSetting(L"EnableThemeSupport") ? PhWindowThemeControlColor(WindowHandle, (HDC)wParam, WindowHandle, CTLCOLOR_DLG) : GetSysColorBrush(COLOR_WINDOW));
        }
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (PhGetIntegerSetting(L"EnableThemeSupport"))
            return (LRESULT)PhWindowThemeControlColor(WindowHandle, (HDC)wParam, (HWND)lParam, WindowMessage == WM_CTLCOLORBTN ? CTLCOLOR_BTN : CTLCOLOR_STATIC);
        break;
    case WM_SETFOCUS:
        if (EaTree)
            SetFocus(EaTree);
        return 0;
    case WM_COMMAND:
        {
            PEA_ROW row = EaGetSelectedRow();
            HWND owner = SystemInformer_GetWindowHandle();

            switch (LOWORD(wParam))
            {
            case EA_ID_EFFICIENCY:
                if (row) EaDoEfficiency(owner, row);
                break;
            case EA_ID_LOWER:
                if (row) EaDoLowerPriority(owner, row);
                break;
            case EA_ID_RULE:
                if (row) EaDoRule(owner, row);
                break;
            case EA_ID_IGNORE:
                if (row) EaDoIgnore(row);
                break;
            case EA_ID_REFRESH:
                EaRebuild(TRUE);
                break;
            }
        }
        break;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

static BOOLEAN EaRegisterClass(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static BOOLEAN ok = FALSE;

    if (PhBeginInitOnce(&initOnce))
    {
        WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };

        wcex.lpfnWndProc = EaTabWndProc;
        wcex.hInstance = PluginInstance->DllBase;
        wcex.hCursor = PhLoadCursor(NULL, IDC_ARROW);
        wcex.lpszClassName = EA_TAB_CLASS;
        ok = RegisterClassEx(&wcex) != 0;
        PhEndInitOnce(&initOnce);
    }

    return ok;
}

static HWND EaCreateButton(
    _In_ PCWSTR Text,
    _In_ ULONG Id
    )
{
    return CreateWindowEx(0, WC_BUTTON, Text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, EaTabWindow, (HMENU)(ULONG_PTR)Id, PluginInstance->DllBase, NULL);
}

static BOOLEAN EaCreateTabWindow(
    _In_ HWND Parent
    )
{
    RECT client = { 0 };
    BOOLEAN themeSupport = !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT);
    ULONG thinRows = PhGetIntegerSetting(SETTING_THIN_ROWS) ? TN_STYLE_THIN_ROWS : 0;
    PH_TREENEW_CREATEPARAMS createParams = { sizeof(PH_TREENEW_CREATEPARAMS) };
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;

    if (!EaRegisterClass())
        return FALSE;

    PhGetClientRect(Parent, &client);
    EaTabWindow = PhCreateWindow(EA_TAB_CLASS, NULL, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, client.left, client.top, client.right - client.left, client.bottom - client.top, Parent, NULL, PluginInstance->DllBase, NULL);

    if (!EaTabWindow)
        return FALSE;

    EaButtons[0] = EaCreateButton(L"Efficiency mode", EA_ID_EFFICIENCY);
    EaButtons[1] = EaCreateButton(L"Lower priority", EA_ID_LOWER);
    EaButtons[2] = EaCreateButton(L"Make a rule...", EA_ID_RULE);
    EaButtons[3] = EaCreateButton(L"Ignore", EA_ID_IGNORE);
    EaRefreshButton = EaCreateButton(L"Refresh", EA_ID_REFRESH);
    EaInfo = CreateWindowEx(0, WC_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, EaTabWindow, (HMENU)(ULONG_PTR)EA_ID_INFO, PluginInstance->DllBase, NULL);
    EaTree = PhCreateWindow(
        PH_TREENEW_CLASSNAME, NULL,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | TN_STYLE_DOUBLE_BUFFERED | thinRows,
        0, 0, 0, 0, EaTabWindow, NULL, NULL, &createParams
        );

    if (!EaButtons[0] || !EaButtons[1] || !EaButtons[2] || !EaButtons[3] || !EaRefreshButton || !EaInfo || !EaTree)
        return FALSE;

    if (themeSupport)
        TreeNew_ThemeSupport(EaTree, TRUE);

    PhSetControlTheme(EaTree, L"explorer");
    TreeNew_SetRedraw(EaTree, FALSE);
    TreeNew_SetCallback(EaTree, EaTreeNewCallback, NULL);
    PhAddTreeNewColumn(EaTree, EA_COL_NAME, TRUE, L"Program", 150, PH_ALIGN_LEFT, EA_COL_NAME, 0);
    PhAddTreeNewColumnEx(EaTree, EA_COL_PID, TRUE, L"PID", 55, PH_ALIGN_RIGHT, EA_COL_PID, DT_RIGHT, TRUE);
    PhAddTreeNewColumn(EaTree, EA_COL_WHY, TRUE, L"Why it is listed", 380, PH_ALIGN_LEFT, EA_COL_WHY, 0);
    PhAddTreeNewColumnEx(EaTree, EA_COL_CPU, TRUE, L"Background CPU", 100, PH_ALIGN_RIGHT, EA_COL_CPU, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(EaTree, EA_COL_MEMORY, TRUE, L"Memory", 80, PH_ALIGN_RIGHT, EA_COL_MEMORY, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(EaTree, EA_COL_DISK, TRUE, L"Disk", 80, PH_ALIGN_RIGHT, EA_COL_DISK, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(EaTree, EA_COL_BACKGROUND, TRUE, L"In background", 100, PH_ALIGN_RIGHT, EA_COL_BACKGROUND, DT_RIGHT, TRUE);
    PhAddTreeNewColumn(EaTree, EA_COL_EFFICIENCY, TRUE, L"Efficiency mode", 100, PH_ALIGN_LEFT, EA_COL_EFFICIENCY, 0);
    PhAddTreeNewColumn(EaTree, EA_COL_PRIORITY, TRUE, L"Priority", 90, PH_ALIGN_LEFT, EA_COL_PRIORITY, 0);
    PhAddTreeNewColumnEx(EaTree, EA_COL_SCORE, TRUE, L"Score", 60, PH_ALIGN_RIGHT, EA_COL_SCORE, DT_RIGHT, TRUE);
    TreeNew_SetEmptyText(EaTree, &EaEmptyText, 0);
    TreeNew_SetTriState(EaTree, TRUE);

    if (PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_ROW_SIZE))
        TreeNew_SetRowHeight(EaTree, max((ULONG)PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_ROW_SIZE), 15));

    TreeNew_SetSort(EaTree, EaSortColumn, EaSortOrder);
    settings = PhGetStringSetting(SETTING_NAME_COLUMNS);
    PhCmLoadSettings(EaTree, &settings->sr);
    PhDereferenceObject(settings);
    sortSettings = PhGetIntegerPairSetting(SETTING_NAME_SORT);

    if ((ULONG)sortSettings.X < EA_COL_MAXIMUM)
        TreeNew_SetSort(EaTree, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);

    TreeNew_SetRedraw(EaTree, TRUE);

    if (themeSupport)
        PhInitializeWindowTheme(EaTabWindow, TRUE);

    EaCreated = TRUE;
    EaUpdateFont();
    EaLayout();
    EaUpdateButtons();
    EaUpdateInfo();
    return TRUE;
}

_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
static BOOLEAN NTAPI EaTabPageCallback(
    _In_ PPH_MAIN_TAB_PAGE Page,
    _In_ PH_MAIN_TAB_PAGE_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case MainTabPageCreateWindow:
        {
            if (!EaCreateTabWindow(Parameter2))
                return FALSE;

            if (Parameter1)
                *(HWND *)Parameter1 = EaTabWindow;
        }
        return TRUE;
    case MainTabPageLoadSettings:
        return TRUE;
    case MainTabPageSaveSettings:
        {
            if (EaCreated)
            {
                PPH_STRING settings = PhCmSaveSettings(EaTree);
                PH_INTEGER_PAIR sortSettings;
                ULONG sortColumn;
                PH_SORT_ORDER sortOrder;

                PhSetStringSetting2(SETTING_NAME_COLUMNS, &settings->sr);
                PhDereferenceObject(settings);
                TreeNew_GetSort(EaTree, &sortColumn, &sortOrder);
                sortSettings.X = sortColumn;
                sortSettings.Y = sortOrder;
                PhSetIntegerPairSetting(SETTING_NAME_SORT, sortSettings);
            }
        }
        return TRUE;
    case MainTabPageSelected:
        {
            EaSelected = !!Parameter1;

            if (EaSelected)
                EaRebuild(TRUE);
        }
        break;
    case MainTabPageFontChanged:
        {
            if (EaCreated)
                SendMessage(EaTree, WM_SETFONT, (WPARAM)Parameter1, TRUE);
        }
        break;
    case MainTabPageDpiChanged:
        {
            if (EaCreated)
            {
                EaUpdateFont();
                EaLayout();
            }
        }
        break;
    }

    return FALSE;
}

VOID EaSelectTab(
    VOID
    )
{
    if (EaTabPage)
        SystemInformer_SelectTabPage(EaTabPage->Index);
}

VOID EaInitializeTab(
    VOID
    )
{
    PH_MAIN_TAB_PAGE page;

    memset(&page, 0, sizeof(PH_MAIN_TAB_PAGE));
    page.Name = EaPageName;
    page.Callback = EaTabPageCallback;
    EaTabPage = PhPluginCreateTabPage(&page);
}

VOID EaTestAction(
    _In_ ULONG Op
    )
{
    PPH_STRING program = PhGetStringSetting(SETTING_NAME_TEST_PROGRAM);
    PPH_LIST rows;
    ULONG i;

    EaLastRebuild = 0;
    rows = EaBuildRows();

    for (i = 0; i < rows->Count; i++)
    {
        PEA_ROW row = rows->Items[i];

        if (row->Name && PhEqualString(row->Name, program, TRUE))
        {
            HWND owner = SystemInformer_GetWindowHandle();

            if (Op == 2)
                EaDoEfficiency(owner, row);
            else if (Op == 3)
                EaDoLowerPriority(owner, row);
            else if (Op == 4)
                EaDoIgnore(row);

            break;
        }
    }

    EaFreeRows(rows);
    PhDereferenceObject(program);
}
