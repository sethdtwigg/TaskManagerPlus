/*
 * TaskManagerPlus ResourceTrends: the Trends tab.
 *
 * Layout (top to bottom): view / time range row, lookup row (date, time, span, rank by), three sparklines
 * (system CPU, physical memory, commit), an info line, and a table that is either "usage at time" (ranked
 * processes around the picked time) or "leak suspects".
 */

#include "resourcetrends.h"
#include <commctrl.h>

#define TRS_TAB_CLASS L"TaskManagerPlusResourceTrendsTab"
#define TRS_SPARK_CLASS L"TaskManagerPlusResourceTrendsSpark"

#define TRS_ID_VIEW 2001
#define TRS_ID_RANGE 2002
#define TRS_ID_DATE 2003
#define TRS_ID_TIME 2004
#define TRS_ID_SPAN 2005
#define TRS_ID_RANK 2006
#define TRS_ID_LOOKUP 2007
#define TRS_ID_LATEST 2008
#define TRS_ID_INFO 2009
#define TRS_ID_SPARK 2010

#define TRS_WM_PICKED (WM_APP + 41) // spark -> tab: lParam = PLONGLONG (FILETIME of the clicked point)

#define TRS_MAX_COLUMNS 9
#define TRS_MAX_ROWS 300

typedef struct _TRS_ROW
{
    PH_TREENEW_NODE Node;
    PH_STRINGREF TextCache[TRS_MAX_COLUMNS];
    PPH_STRING Cells[TRS_MAX_COLUMNS];
    double Keys[TRS_MAX_COLUMNS];
    ULONG ProcessId;
    LONGLONG CreateTime;
} TRS_ROW, *PTRS_ROW;

typedef struct _TRS_TABLE
{
    HWND Handle;
    PPH_LIST Rows;
    ULONG ColumnCount;
    BOOLEAN TextColumn[TRS_MAX_COLUMNS];
    ULONG SortColumn;
    PH_SORT_ORDER SortOrder;
    PCWSTR ColumnsSetting;
    PCWSTR SortSetting;
} TRS_TABLE, *PTRS_TABLE;

typedef struct _TRS_COLORS
{
    COLORREF Back;
    COLORREF Text;
    COLORREF Dim;
    COLORREF Grid;
    COLORREF Cpu;
    COLORREF Memory;
    COLORREF Commit;
    COLORREF Cursor;
} TRS_COLORS;

static CONST PH_STRINGREF TrsTabPageText = PH_STRINGREF_INIT(L"Trends");
static CONST PH_STRINGREF TrsUsageEmptyText = PH_STRINGREF_INIT(L"No samples in this time span yet.");
static CONST PH_STRINGREF TrsLeakEmptyText = PH_STRINGREF_INIT(L"No process is growing steadily right now.");
static CONST LONGLONG TrsRangeSeconds[] = { 3600, 6 * 3600, 24 * 3600, 7 * 24 * 3600, 0 };
static CONST ULONG TrsSpanSeconds[] = { 60, 300, 900, 3600 };

static PPH_MAIN_TAB_PAGE TrsTabPage = NULL;
static HWND TrsTabWindow = NULL;
static HWND TrsViewCombo = NULL;
static HWND TrsRangeCombo = NULL;
static HWND TrsDateControl = NULL;
static HWND TrsTimeControl = NULL;
static HWND TrsSpanCombo = NULL;
static HWND TrsRankCombo = NULL;
static HWND TrsLookupButton = NULL;
static HWND TrsLatestButton = NULL;
static HWND TrsInfoLabel = NULL;
static HWND TrsSparkWindow = NULL;
static HFONT TrsFont = NULL;
static BOOLEAN TrsCreated = FALSE;
static TRS_TABLE TrsUsageTable;
static TRS_TABLE TrsLeakTable;
static LONGLONG TrsLookupCenter = 0; // FILETIME (UTC)
static BOOLEAN TrsFollowLatest = TRUE;
static BOOLEAN TrsUpdatingControls = FALSE;
static PTRS_TABLE TrsSortTable = NULL;

static VOID TrsRefreshAll(VOID);

// ---------------------------------------------------------------- helpers

static PPH_STRING TrsFormatLocal(
    _In_ LONGLONG FileTime,
    _In_ BOOLEAN WithDate
    )
{
    FILETIME ft;
    SYSTEMTIME utc;
    SYSTEMTIME local;

    ft.dwLowDateTime = (ULONG)FileTime;
    ft.dwHighDateTime = (ULONG)(FileTime >> 32);

    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(NULL, &utc, &local))
        return PhCreateString(L"?");

    if (WithDate)
        return PhFormatString(L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);

    return PhFormatString(L"%02u:%02u:%02u", local.wHour, local.wMinute, local.wSecond);
}

static LONGLONG TrsLocalToFileTime(
    _In_ PSYSTEMTIME Local
    )
{
    SYSTEMTIME utc;
    FILETIME ft;

    if (!TzSpecificLocalTimeToSystemTime(NULL, Local, &utc) || !SystemTimeToFileTime(&utc, &ft))
        return 0;

    return (LONGLONG)ft.dwLowDateTime | ((LONGLONG)ft.dwHighDateTime << 32);
}

static BOOLEAN TrsFileTimeToLocal(
    _In_ LONGLONG FileTime,
    _Out_ PSYSTEMTIME Local
    )
{
    FILETIME ft;
    SYSTEMTIME utc;

    ft.dwLowDateTime = (ULONG)FileTime;
    ft.dwHighDateTime = (ULONG)(FileTime >> 32);
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, Local);
}

static LONGLONG TrsLatestSampleTime(
    VOID
    )
{
    ULONG count = TrsStoreGetSampleCount();

    return count ? TrsStoreGetSample(count - 1)->System.Time : 0;
}

static BOOLEAN TrsGetColors(
    _Out_ TRS_COLORS *Colors
    )
{
    BOOLEAN dark;

    if (PhGetIntegerSetting(L"EnableThemeSupport"))
    {
        const PH_WINDOW_THEME_PALETTE *palette = PhGetWindowThemePalette();

        Colors->Back = palette->BackgroundColor;
        Colors->Text = palette->TextColor;
        Colors->Dim = palette->DisabledTextColor;
        Colors->Grid = palette->BorderColor;
    }
    else
    {
        Colors->Back = GetSysColor(COLOR_WINDOW);
        Colors->Text = GetSysColor(COLOR_WINDOWTEXT);
        Colors->Dim = GetSysColor(COLOR_GRAYTEXT);
        Colors->Grid = GetSysColor(COLOR_3DLIGHT);
    }

    dark = (GetRValue(Colors->Back) * 299 + GetGValue(Colors->Back) * 587 + GetBValue(Colors->Back) * 114) / 1000 < 110;

    if (dark)
    {
        Colors->Cpu = RGB(96, 165, 250);
        Colors->Memory = RGB(74, 222, 128);
        Colors->Commit = RGB(251, 146, 60);
        Colors->Cursor = RGB(250, 204, 21);
    }
    else
    {
        Colors->Cpu = RGB(37, 99, 235);
        Colors->Memory = RGB(22, 163, 74);
        Colors->Commit = RGB(234, 88, 12);
        Colors->Cursor = RGB(202, 138, 4);
    }

    return dark;
}

static COLORREF TrsMix(
    _In_ COLORREF A,
    _In_ COLORREF B,
    _In_ ULONG PercentB
    )
{
    return RGB(
        (GetRValue(A) * (100 - PercentB) + GetRValue(B) * PercentB) / 100,
        (GetGValue(A) * (100 - PercentB) + GetGValue(B) * PercentB) / 100,
        (GetBValue(A) * (100 - PercentB) + GetBValue(B) * PercentB) / 100
        );
}

// ---------------------------------------------------------------- tables

static VOID TrsFreeRow(
    _In_ PTRS_ROW Row
    )
{
    ULONG i;

    for (i = 0; i < TRS_MAX_COLUMNS; i++)
        PhClearReference(&Row->Cells[i]);

    PhFree(Row);
}

static VOID TrsClearRows(
    _In_ PTRS_TABLE Table
    )
{
    ULONG i;

    for (i = 0; i < Table->Rows->Count; i++)
        TrsFreeRow(Table->Rows->Items[i]);

    PhClearList(Table->Rows);
}

static PTRS_ROW TrsAddRow(
    _In_ PTRS_TABLE Table,
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    )
{
    PTRS_ROW row = PhAllocateZero(sizeof(TRS_ROW));

    PhInitializeTreeNewNode(&row->Node);
    row->Node.TextCache = row->TextCache;
    row->Node.TextCacheSize = TRS_MAX_COLUMNS;
    row->ProcessId = ProcessId;
    row->CreateTime = CreateTime;
    PhAddItemList(Table->Rows, row);
    return row;
}

static VOID TrsSetCell(
    _Inout_ PTRS_ROW Row,
    _In_ ULONG Column,
    _In_ PPH_STRING Text, // consumed
    _In_ double Key
    )
{
    PhMoveReference(&Row->Cells[Column], Text);
    Row->Keys[Column] = Key;
}

static int __cdecl TrsRowCompare(
    _In_ const void *A,
    _In_ const void *B
    )
{
    PTRS_ROW a = *(PTRS_ROW *)A;
    PTRS_ROW b = *(PTRS_ROW *)B;
    ULONG column = TrsSortTable->SortColumn;
    int result;

    if (column >= TrsSortTable->ColumnCount)
        return 0;

    if (TrsSortTable->TextColumn[column])
        result = PhCompareStringWithNull(a->Cells[column], b->Cells[column], TRUE);
    else
        result = a->Keys[column] < b->Keys[column] ? -1 : (a->Keys[column] > b->Keys[column] ? 1 : 0);

    return PhModifySort(result, TrsSortTable->SortOrder);
}

static VOID TrsGoToProcess(
    _In_ PTRS_ROW Row
    )
{
    PPH_PROCESS_NODE processNode;

    if (!Row->ProcessId)
        return;

    processNode = PhFindProcessNode(UlongToHandle(Row->ProcessId));

    if (processNode && processNode->ProcessItem && processNode->ProcessItem->CreateTime.QuadPart == Row->CreateTime)
    {
        SystemInformer_SelectTabPage(0);
        SystemInformer_SelectProcessNode(processNode);
    }
    else
    {
        PhShowInformation2(
            TrsTabWindow,
            L"Resource Trends",
            L"%s (PID %lu) is no longer running.",
            PhGetStringOrDefault(Row->Cells[0], L"The process"),
            Row->ProcessId
            );
    }
}

static PTRS_ROW TrsGetSelectedRow(
    _In_ PTRS_TABLE Table
    )
{
    ULONG i;

    for (i = 0; i < Table->Rows->Count; i++)
    {
        PTRS_ROW row = Table->Rows->Items[i];

        if (row->Node.Selected)
            return row;
    }

    return NULL;
}

static VOID TrsCopyRow(
    _In_ PTRS_TABLE Table
    )
{
    PTRS_ROW row = TrsGetSelectedRow(Table);
    PH_STRING_BUILDER sb;
    ULONG i;
    PPH_STRING text;

    if (!row)
        return;

    PhInitializeStringBuilder(&sb, 128);

    for (i = 0; i < Table->ColumnCount; i++)
    {
        if (i)
            PhAppendCharStringBuilder(&sb, L'\t');

        {
            PH_STRINGREF cell = PhGetStringRef(row->Cells[i]);

            PhAppendStringBuilder(&sb, &cell);
        }
    }

    text = PhFinalStringBuilderString(&sb);
    PhSetClipboardString(TrsTabWindow, &text->sr);
    PhDereferenceObject(text);
}

#define TRS_MENU_GOTO 1
#define TRS_MENU_COPY 2

static VOID TrsShowContextMenu(
    _In_ PTRS_TABLE Table,
    _In_ PPH_TREENEW_CONTEXT_MENU ContextMenuEvent
    )
{
    PPH_EMENU menu;
    PPH_EMENU_ITEM selected;
    PTRS_ROW row = TrsGetSelectedRow(Table);

    if (!row)
        return;

    menu = PhCreateEMenu();
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TRS_MENU_GOTO, L"&Go to process", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TRS_MENU_COPY, L"&Copy row", NULL, NULL), ULONG_MAX);
    PhSetFlagsEMenuItem(menu, TRS_MENU_GOTO, PH_EMENU_DEFAULT, PH_EMENU_DEFAULT);

    if (!row->ProcessId)
        PhEnableEMenuItem(menu, TRS_MENU_GOTO, FALSE);

    selected = PhShowEMenu(menu, Table->Handle, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, ContextMenuEvent->Location.x, ContextMenuEvent->Location.y);

    if (selected && selected->Id == TRS_MENU_GOTO)
        TrsGoToProcess(row);
    else if (selected && selected->Id == TRS_MENU_COPY)
        TrsCopyRow(Table);

    PhDestroyEMenu(menu);
}

static BOOLEAN NTAPI TrsTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_opt_ PVOID Context
    )
{
    PTRS_TABLE table = Context;

    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;

            if (getChildren->Node)
                break;

            if (table->SortOrder != NoSortOrder && table->Rows->Count > 1)
            {
                TrsSortTable = table;
                qsort(table->Rows->Items, table->Rows->Count, sizeof(PVOID), TrsRowCompare);
            }

            getChildren->Children = (PPH_TREENEW_NODE *)table->Rows->Items;
            getChildren->NumberOfChildren = table->Rows->Count;
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
            PTRS_ROW row = (PTRS_ROW)getCellText->Node;

            if (getCellText->Id >= table->ColumnCount)
                return FALSE;

            getCellText->Text = PhGetStringRef(row->Cells[getCellText->Id]);
            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewSortChanged:
        {
            PPH_TREENEW_SORT_CHANGED_EVENT sorting = Parameter1;

            table->SortColumn = sorting->SortColumn;
            table->SortOrder = sorting->SortOrder;
            TreeNew_NodesStructured(WindowHandle);
        }
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            if (keyEvent->VirtualKey == 'C' && GetKeyState(VK_CONTROL) < 0)
                TrsCopyRow(table);
            else if (keyEvent->VirtualKey == VK_RETURN)
            {
                PTRS_ROW row = TrsGetSelectedRow(table);

                if (row)
                    TrsGoToProcess(row);
            }
        }
        return TRUE;
    case TreeNewLeftDoubleClick:
        {
            PTRS_ROW row = TrsGetSelectedRow(table);

            if (row)
                TrsGoToProcess(row);
        }
        return TRUE;
    case TreeNewContextMenu:
        TrsShowContextMenu(table, Parameter1);
        return TRUE;
    case TreeNewHeaderRightClick:
        {
            PH_TN_COLUMN_MENU_DATA data;

            data.TreeNewHandle = WindowHandle;
            data.MouseEvent = Parameter1;
            data.DefaultSortColumn = 0;
            data.DefaultSortOrder = AscendingSortOrder;
            PhInitializeTreeNewColumnMenuEx(&data, PH_TN_COLUMN_MENU_SHOW_RESET_SORT);
            data.Selection = PhShowEMenu(data.Menu, WindowHandle, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, data.MouseEvent->ScreenLocation.x, data.MouseEvent->ScreenLocation.y);
            PhHandleTreeNewColumnMenu(&data);
            PhDeleteTreeNewColumnMenu(&data);
        }
        return TRUE;
    }

    return FALSE;
}

static HWND TrsCreateTreeNew(
    _In_ HWND Parent
    )
{
    ULONG thinRows = PhGetIntegerSetting(SETTING_THIN_ROWS) ? TN_STYLE_THIN_ROWS : 0;
    BOOLEAN themeSupport = !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT);
    ULONG border = (PhGetIntegerSetting(SETTING_TREE_LIST_BORDER_ENABLE) && !themeSupport) ? WS_BORDER : 0;
    PH_TREENEW_CREATEPARAMS createParams = { sizeof(PH_TREENEW_CREATEPARAMS) };
    HWND handle;

    handle = PhCreateWindow(
        PH_TREENEW_CLASSNAME,
        NULL,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | TN_STYLE_DOUBLE_BUFFERED | thinRows | border,
        0, 0, 0, 0,
        Parent,
        NULL,
        NULL,
        &createParams
        );

    if (handle && themeSupport)
        TreeNew_ThemeSupport(handle, TRUE);

    return handle;
}

static VOID TrsInitializeTable(
    _Inout_ PTRS_TABLE Table,
    _In_ HWND Parent,
    _In_ PCWSTR ColumnsSetting,
    _In_ PCWSTR SortSetting,
    _In_ PCPH_STRINGREF EmptyText,
    _In_ ULONG DefaultSortColumn
    )
{
    memset(Table, 0, sizeof(TRS_TABLE));
    Table->Rows = PhCreateList(256);
    Table->ColumnsSetting = ColumnsSetting;
    Table->SortSetting = SortSetting;
    Table->Handle = TrsCreateTreeNew(Parent);
    Table->SortColumn = DefaultSortColumn;
    Table->SortOrder = DescendingSortOrder;
    (VOID)EmptyText;
}

static VOID TrsFinishTable(
    _Inout_ PTRS_TABLE Table,
    _In_ PCPH_STRINGREF EmptyText
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;

    PhSetControlTheme(Table->Handle, L"explorer");
    TreeNew_SetCallback(Table->Handle, TrsTreeNewCallback, Table);
    TreeNew_SetEmptyText(Table->Handle, EmptyText, 0);
    TreeNew_SetTriState(Table->Handle, TRUE);

    if (PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_ROW_SIZE))
    {
        ULONG rowSize = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_ROW_SIZE);

        TreeNew_SetRowHeight(Table->Handle, max(rowSize, 15));
    }

    TreeNew_SetSort(Table->Handle, Table->SortColumn, Table->SortOrder);

    settings = PhGetStringSetting(Table->ColumnsSetting);
    PhCmLoadSettings(Table->Handle, &settings->sr);
    PhDereferenceObject(settings);

    sortSettings = PhGetIntegerPairSetting(Table->SortSetting);

    if ((ULONG)sortSettings.X < Table->ColumnCount)
        TreeNew_SetSort(Table->Handle, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);
}

static VOID TrsSaveTable(
    _In_ PTRS_TABLE Table
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;
    ULONG sortColumn;
    PH_SORT_ORDER sortOrder;

    if (!Table->Handle)
        return;

    settings = PhCmSaveSettings(Table->Handle);
    PhSetStringSetting2(Table->ColumnsSetting, &settings->sr);
    PhDereferenceObject(settings);

    TreeNew_GetSort(Table->Handle, &sortColumn, &sortOrder);
    sortSettings.X = sortColumn;
    sortSettings.Y = sortOrder;
    PhSetIntegerPairSetting(Table->SortSetting, sortSettings);
}

static VOID TrsReselect(
    _In_ PTRS_TABLE Table,
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    )
{
    ULONG i;

    if (!ProcessId && !CreateTime)
        return;

    for (i = 0; i < Table->Rows->Count; i++)
    {
        PTRS_ROW row = Table->Rows->Items[i];

        if (row->ProcessId == ProcessId && row->CreateTime == CreateTime)
        {
            row->Node.Selected = TRUE;
            TreeNew_InvalidateNode(Table->Handle, &row->Node);
            break;
        }
    }
}

// Usage-at-time table

enum { TRS_UC_NAME, TRS_UC_PID, TRS_UC_AVGCPU, TRS_UC_PEAKCPU, TRS_UC_AVGPRIV, TRS_UC_PEAKPRIV, TRS_UC_PEAKWS, TRS_UC_SAMPLES, TRS_UC_STATUS };
enum { TRS_LC_NAME, TRS_LC_PID, TRS_LC_CURRENT, TRS_LC_GROWTH, TRS_LC_RATE, TRS_LC_WINDOW, TRS_LC_STEADY };

static PPH_STRING TrsFormatKB(
    _In_ ULONG64 KB
    )
{
    return PhFormatSize(KB * 1024, ULONG_MAX);
}

static PPH_STRING TrsFormatDuration(
    _In_ ULONG Seconds
    )
{
    if (Seconds < 120)
        return PhFormatString(L"%lu s", Seconds);

    return PhFormatString(L"%lu min", Seconds / 60);
}

static VOID TrsRefreshUsage(
    VOID
    )
{
    LONGLONG from;
    LONGLONG to;
    LONGLONG span;
    ULONG spanIndex;
    TR_AGGREGATE *rows = NULL;
    ULONG inRange = 0;
    ULONG count;
    ULONG i;
    ULONG sampleCount = TrsStoreGetSampleCount();
    ULONG selectedPid = 0;
    LONGLONG selectedCreate = 0;
    PTRS_ROW selected = TrsGetSelectedRow(&TrsUsageTable);
    double cpuSum = 0;
    double memSum = 0;
    double commitSum = 0;
    ULONG n = 0;
    PPH_STRING fromText;
    PPH_STRING toText;
    PPH_STRING info;

    if (selected)
    {
        selectedPid = selected->ProcessId;
        selectedCreate = selected->CreateTime;
    }

    spanIndex = (ULONG)SendMessage(TrsSpanCombo, CB_GETCURSEL, 0, 0);

    if (spanIndex >= RTL_NUMBER_OF(TrsSpanSeconds))
        spanIndex = 1;

    span = (LONGLONG)TrsSpanSeconds[spanIndex] * TR_FILETIME_SECOND;
    from = TrsLookupCenter - span;
    to = TrsLookupCenter + span;

    TrsClearRows(&TrsUsageTable);

    if (sampleCount)
        count = TrAggregate(TrsStoreGetSamples(), sampleCount, from, to, 0, &rows, &inRange);
    else
        count = 0;

    if (count > TRS_MAX_ROWS)
        count = TRS_MAX_ROWS;

    for (i = 0; i < count; i++)
    {
        PTRS_ROW row = TrsAddRow(&TrsUsageTable, rows[i].ProcessId, rows[i].CreateTime);
        PPH_STRING name = TrsStoreGetName(rows[i].NameIndex);
        PPH_PROCESS_NODE live = PhFindProcessNode(UlongToHandle(rows[i].ProcessId));
        BOOLEAN running = live && live->ProcessItem && live->ProcessItem->CreateTime.QuadPart == rows[i].CreateTime;

        TrsSetCell(row, TRS_UC_NAME, name ? PhReferenceObject(name) : PhCreateString(L"?"), 0);
        TrsSetCell(row, TRS_UC_PID, PhFormatString(L"%lu", rows[i].ProcessId), rows[i].ProcessId);
        TrsSetCell(row, TRS_UC_AVGCPU, PhFormatString(L"%.2f%%", rows[i].AvgCpuPercent), rows[i].AvgCpuPercent);
        TrsSetCell(row, TRS_UC_PEAKCPU, PhFormatString(L"%.2f%%", rows[i].PeakCpuPercent), rows[i].PeakCpuPercent);
        TrsSetCell(row, TRS_UC_AVGPRIV, TrsFormatKB(rows[i].AvgPrivateKB), rows[i].AvgPrivateKB);
        TrsSetCell(row, TRS_UC_PEAKPRIV, TrsFormatKB(rows[i].PeakPrivateKB), rows[i].PeakPrivateKB);
        TrsSetCell(row, TRS_UC_PEAKWS, TrsFormatKB(rows[i].PeakWorkingSetKB), rows[i].PeakWorkingSetKB);
        TrsSetCell(row, TRS_UC_SAMPLES, PhFormatString(L"%lu", rows[i].Samples), rows[i].Samples);
        TrsSetCell(row, TRS_UC_STATUS, PhCreateString(running ? L"Running" : L"Exited"), running ? 1 : 0);
    }

    TrFreeAggregate(rows);

    for (i = 0; i < sampleCount; i++)
    {
        TR_SAMPLE *sample = TrsStoreGetSample(i);

        if (sample->System.Time >= from && sample->System.Time <= to)
        {
            cpuSum += sample->System.CpuX100 / 100.0;
            memSum += sample->System.PhysicalTotalMB ? 100.0 * sample->System.PhysicalUsedMB / sample->System.PhysicalTotalMB : 0;
            commitSum += sample->System.CommitLimitMB ? 100.0 * sample->System.CommitUsedMB / sample->System.CommitLimitMB : 0;
            n++;
        }
    }

    fromText = TrsFormatLocal(from, TRUE);
    toText = TrsFormatLocal(to, TRUE);

    if (n)
    {
        info = PhFormatString(
            L"%lu sample%s between %s and %s (local time). System averages: CPU %.1f%%, memory %.1f%%, commit %.1f%%. Process CPU is a share of the whole system; only the busiest and largest processes are recorded in each sample.",
            n,
            n == 1 ? L"" : L"s",
            fromText->Buffer,
            toText->Buffer,
            cpuSum / n,
            memSum / n,
            commitSum / n
            );
    }
    else
    {
        info = PhFormatString(L"No samples between %s and %s (local time). Pick another time or a wider span.", fromText->Buffer, toText->Buffer);
    }

    SetWindowText(TrsInfoLabel, info->Buffer);
    PhDereferenceObject(info);
    PhDereferenceObject(fromText);
    PhDereferenceObject(toText);

    TreeNew_NodesStructured(TrsUsageTable.Handle);
    TrsReselect(&TrsUsageTable, selectedPid, selectedCreate);
}

static VOID TrsRefreshLeaks(
    VOID
    )
{
    ULONG i;
    ULONG selectedPid = 0;
    LONGLONG selectedCreate = 0;
    PTRS_ROW selected = TrsGetSelectedRow(&TrsLeakTable);
    LK_CONFIG config;

    if (selected)
    {
        selectedPid = selected->ProcessId;
        selectedCreate = selected->CreateTime;
    }

    TrsGetLeakConfig(&config);
    TrsClearRows(&TrsLeakTable);

    for (i = 0; i < TrsGetSuspectCount(); i++)
    {
        PTRS_SUSPECT suspect = TrsGetSuspect(i);
        PTRS_ROW row = TrsAddRow(&TrsLeakTable, suspect->ProcessId, suspect->CreateTime);

        TrsSetCell(row, TRS_LC_NAME, PhReferenceObject(suspect->Name), 0);
        TrsSetCell(row, TRS_LC_PID, suspect->ProcessId ? PhFormatString(L"%lu", suspect->ProcessId) : PhCreateString(L""), suspect->ProcessId);
        TrsSetCell(row, TRS_LC_CURRENT, TrsFormatKB(suspect->CurrentKB), (double)suspect->CurrentKB);
        TrsSetCell(row, TRS_LC_GROWTH, PhFormatString(L"+%s", TrsFormatKB(suspect->Result.GrowthKB)->Buffer), (double)suspect->Result.GrowthKB);
        TrsSetCell(row, TRS_LC_RATE, PhFormatString(L"%s/min", TrsFormatKB((ULONG64)suspect->Result.RateKBPerMinute)->Buffer), suspect->Result.RateKBPerMinute);
        TrsSetCell(row, TRS_LC_WINDOW, TrsFormatDuration((ULONG)suspect->Result.SpanSeconds), (double)suspect->Result.SpanSeconds);
        TrsSetCell(row, TRS_LC_STEADY, PhFormatString(L"%lu%%", suspect->Result.UpPercent), suspect->Result.UpPercent);
    }

    TreeNew_NodesStructured(TrsLeakTable.Handle);
    TrsReselect(&TrsLeakTable, selectedPid, selectedCreate);
}

static BOOLEAN TrsLeakViewActive(
    VOID
    )
{
    return SendMessage(TrsViewCombo, CB_GETCURSEL, 0, 0) == 1;
}

static VOID TrsSetControlsFromCenter(
    VOID
    )
{
    SYSTEMTIME local;

    if (!TrsLookupCenter || !TrsFileTimeToLocal(TrsLookupCenter, &local))
        return;

    TrsUpdatingControls = TRUE;
    DateTime_SetSystemtime(TrsDateControl, GDT_VALID, &local);
    DateTime_SetSystemtime(TrsTimeControl, GDT_VALID, &local);
    TrsUpdatingControls = FALSE;
}

static VOID TrsReadCenterFromControls(
    VOID
    )
{
    SYSTEMTIME date;
    SYSTEMTIME time;
    SYSTEMTIME combined;
    LONGLONG value;

    if (DateTime_GetSystemtime(TrsDateControl, &date) != GDT_VALID || DateTime_GetSystemtime(TrsTimeControl, &time) != GDT_VALID)
        return;

    combined = date;
    combined.wHour = time.wHour;
    combined.wMinute = time.wMinute;
    combined.wSecond = time.wSecond;
    combined.wMilliseconds = 0;
    value = TrsLocalToFileTime(&combined);

    if (value)
    {
        TrsLookupCenter = value;
        TrsFollowLatest = FALSE;
    }
}

static VOID TrsUpdateControlState(
    VOID
    )
{
    BOOLEAN usage = !TrsLeakViewActive();

    EnableWindow(TrsDateControl, usage);
    EnableWindow(TrsTimeControl, usage);
    EnableWindow(TrsSpanCombo, usage);
    EnableWindow(TrsRankCombo, usage);
    EnableWindow(TrsLookupButton, usage);
    EnableWindow(TrsLatestButton, usage);
    ShowWindow(TrsUsageTable.Handle, usage ? SW_SHOW : SW_HIDE);
    ShowWindow(TrsLeakTable.Handle, usage ? SW_HIDE : SW_SHOW);
}

static VOID TrsUpdateLeakInfo(
    VOID
    )
{
    LK_CONFIG config;
    PPH_STRING window;
    PPH_STRING growth;
    PPH_STRING text;

    TrsGetLeakConfig(&config);
    growth = TrsFormatKB(config.MinGrowthKB);
    window = TrsFormatDuration((ULONG)config.WindowSeconds);
    text = PhFormatString(
        L"%s. Flags a process when its private bytes (commit) grow by %s (%lu%%) over %s with few dips; change this in Options > Resource Trends.",
        PhGetIntegerSetting(SETTING_NAME_LEAK_ENABLED) ? (TrsGetSuspectCount() ? L"Leak suspects" : L"No leak suspects") : L"Leak detector is off",
        growth->Buffer,
        config.MinGrowthPercent,
        window->Buffer
        );
    PhDereferenceObject(window);
    SetWindowText(TrsInfoLabel, text->Buffer);
    PhDereferenceObject(text);
    PhDereferenceObject(growth);
}

static VOID TrsRefreshAll(
    VOID
    )
{
    if (!TrsCreated)
        return;

    if (TrsFollowLatest && TrsStoreGetSampleCount())
    {
        TrsLookupCenter = TrsLatestSampleTime();
        TrsSetControlsFromCenter();
    }
    else if (!TrsLookupCenter)
    {
        TrsLookupCenter = TrsLatestSampleTime();
        TrsSetControlsFromCenter();
    }

    TrsRefreshLeaks();

    if (TrsLeakViewActive())
    {
        TrsUpdateLeakInfo();
    }
    else
    {
        TrsRefreshUsage();
    }

    InvalidateRect(TrsSparkWindow, NULL, FALSE);
}

VOID TrsTabDataChanged(
    VOID
    )
{
    if (TrsCreated)
        TrsRefreshAll();
}

// ---------------------------------------------------------------- sparklines

typedef struct _TRS_SPARK_STATE
{
    LONGLONG HoverTime; // 0 = none
    LONG HoverX;
} TRS_SPARK_STATE;

static TRS_SPARK_STATE TrsSpark = { 0, -1 };

static VOID TrsGetRange(
    _Out_ LONGLONG *Start,
    _Out_ LONGLONG *End
    )
{
    ULONG count = TrsStoreGetSampleCount();
    ULONG index = (ULONG)SendMessage(TrsRangeCombo, CB_GETCURSEL, 0, 0);
    LONGLONG end = count ? TrsLatestSampleTime() : 0;
    LONGLONG start = count ? TrsStoreGetSample(0)->System.Time : 0;

    if (index < RTL_NUMBER_OF(TrsRangeSeconds) && TrsRangeSeconds[index])
    {
        LONGLONG wanted = end - TrsRangeSeconds[index] * TR_FILETIME_SECOND;

        if (wanted > start)
            start = wanted;
        else if (count)
            start = min(start, end - TrsRangeSeconds[index] * TR_FILETIME_SECOND / 4); // young history: still show it with some room
    }

    if (end <= start)
        end = start + TR_FILETIME_SECOND;

    *Start = start;
    *End = end;
}

static double TrsSeriesValue(
    _In_ const TR_SYSTEM *System,
    _In_ ULONG Series
    )
{
    switch (Series)
    {
    case 0:
        return System->CpuX100 / 10000.0;
    case 1:
        return System->PhysicalTotalMB ? (double)System->PhysicalUsedMB / System->PhysicalTotalMB : 0;
    default:
        return System->CommitLimitMB ? (double)System->CommitUsedMB / System->CommitLimitMB : 0;
    }
}

static VOID TrsPaintSpark(
    _In_ HWND WindowHandle,
    _In_ HDC Dc
    )
{
    RECT client;
    TRS_COLORS colors;
    HBRUSH brush;
    HPEN pen;
    HPEN oldPen;
    HFONT oldFont;
    ULONG count = TrsStoreGetSampleCount();
    LONGLONG start;
    LONGLONG end;
    LONG dpi = PhGetWindowDpi(WindowHandle);
    LONG pad = PhScaleToDisplay(8, dpi);
    LONG headerHeight = PhScaleToDisplay(18, dpi);
    LONG stripHeight;
    ULONG series;
    static PCWSTR names[3] = { L"CPU", L"Memory", L"Commit" };
    COLORREF lineColors[3];

    GetClientRect(WindowHandle, &client);
    TrsGetColors(&colors);
    lineColors[0] = colors.Cpu;
    lineColors[1] = colors.Memory;
    lineColors[2] = colors.Commit;

    brush = CreateSolidBrush(colors.Back);
    FillRect(Dc, &client, brush);
    DeleteObject(brush);

    oldFont = TrsFont ? SelectObject(Dc, TrsFont) : NULL;
    SetBkMode(Dc, TRANSPARENT);

    if (!count)
    {
        RECT text = client;

        SetTextColor(Dc, colors.Dim);
        DrawText(Dc, L"No samples yet. The first sample is taken a moment after start-up and then every sample interval.", -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (oldFont)
            SelectObject(Dc, oldFont);

        return;
    }

    TrsGetRange(&start, &end);
    stripHeight = (client.bottom - client.top) / 3;

    for (series = 0; series < 3; series++)
    {
        RECT strip = { client.left + pad, client.top + (LONG)series * stripHeight + PhScaleToDisplay(2, dpi), client.right - pad, client.top + (LONG)(series + 1) * stripHeight - PhScaleToDisplay(2, dpi) };
        RECT plot = { strip.left, strip.top + headerHeight, strip.right, strip.bottom };
        LONG plotWidth = plot.right - plot.left;
        LONG plotHeight = plot.bottom - plot.top;
        ULONG i;
        ULONG previousIndex = ULONG_MAX;
        POINT previousPoint = { 0, 0 };
        double lastValue = 0;
        double peak = 0;
        double hoverValue = -1;
        PPH_STRING header;
        COLORREF fill = TrsMix(colors.Back, lineColors[series], 28);
        HBRUSH fillBrush = CreateSolidBrush(fill);
        HBRUSH oldBrush;
        RECT textRect;

        if (plotWidth <= 4 || plotHeight <= 4)
            continue;

        // frame and 50 percent guide
        pen = CreatePen(PS_SOLID, 1, colors.Grid);
        oldPen = SelectObject(Dc, pen);
        MoveToEx(Dc, plot.left, plot.bottom - 1, NULL);
        LineTo(Dc, plot.right, plot.bottom - 1);
        MoveToEx(Dc, plot.left, plot.top + plotHeight / 2, NULL);
        LineTo(Dc, plot.right, plot.top + plotHeight / 2);
        SelectObject(Dc, oldPen);
        DeleteObject(pen);

        // lookup span
        if (TrsLookupCenter && !TrsLeakViewActive())
        {
            ULONG spanIndex = (ULONG)SendMessage(TrsSpanCombo, CB_GETCURSEL, 0, 0);
            LONGLONG span = (LONGLONG)(spanIndex < RTL_NUMBER_OF(TrsSpanSeconds) ? TrsSpanSeconds[spanIndex] : 300) * TR_FILETIME_SECOND;
            double x0 = plot.left + (double)(TrsLookupCenter - span - start) * plotWidth / (end - start);
            double x1 = plot.left + (double)(TrsLookupCenter + span - start) * plotWidth / (end - start);
            RECT band;

            band.top = plot.top;
            band.bottom = plot.bottom;
            band.left = (LONG)max(x0, plot.left);
            band.right = (LONG)min(x1, plot.right);

            if (band.right > band.left)
            {
                HBRUSH bandBrush = CreateSolidBrush(TrsMix(colors.Back, colors.Cursor, 12));

                FillRect(Dc, &band, bandBrush);
                DeleteObject(bandBrush);
            }
        }

        oldBrush = SelectObject(Dc, fillBrush);
        pen = CreatePen(PS_SOLID, max(1, PhScaleToDisplay(1, dpi)), lineColors[series]);
        oldPen = SelectObject(Dc, pen);

        for (i = 0; i < count; i++)
        {
            TR_SAMPLE *sample = TrsStoreGetSample(i);
            double value;
            POINT point;

            if (sample->System.Time < start)
                continue;

            value = TrsSeriesValue(&sample->System, series);

            if (value > 1)
                value = 1;

            lastValue = value;

            if (value > peak)
                peak = value;

            if (TrsSpark.HoverTime && hoverValue < 0 && sample->System.Time >= TrsSpark.HoverTime)
                hoverValue = value;

            point.x = plot.left + (LONG)((double)(sample->System.Time - start) * plotWidth / (end - start));
            point.y = plot.bottom - 1 - (LONG)(value * (plotHeight - 2));

            if (previousIndex != ULONG_MAX && sample->System.Time - TrsStoreGetSample(previousIndex)->System.Time <= 45 * TR_FILETIME_MINUTE)
            {
                POINT polygon[4];

                polygon[0] = previousPoint;
                polygon[1] = point;
                polygon[2].x = point.x;
                polygon[2].y = plot.bottom - 1;
                polygon[3].x = previousPoint.x;
                polygon[3].y = plot.bottom - 1;
                SelectObject(Dc, GetStockObject(NULL_PEN));
                Polygon(Dc, polygon, 4);
                SelectObject(Dc, pen);
                MoveToEx(Dc, previousPoint.x, previousPoint.y, NULL);
                LineTo(Dc, point.x, point.y);
            }

            previousIndex = i;
            previousPoint = point;
        }

        SelectObject(Dc, oldPen);
        DeleteObject(pen);
        SelectObject(Dc, oldBrush);
        DeleteObject(fillBrush);

        // lookup cursor
        if (TrsLookupCenter && TrsLookupCenter >= start && TrsLookupCenter <= end && !TrsLeakViewActive())
        {
            LONG x = plot.left + (LONG)((double)(TrsLookupCenter - start) * plotWidth / (end - start));

            pen = CreatePen(PS_SOLID, max(2, PhScaleToDisplay(2, dpi)), colors.Cursor);
            oldPen = SelectObject(Dc, pen);
            MoveToEx(Dc, x, plot.top, NULL);
            LineTo(Dc, x, plot.bottom);
            SelectObject(Dc, oldPen);
            DeleteObject(pen);
        }

        // hover line
        if (TrsSpark.HoverTime && TrsSpark.HoverX >= plot.left && TrsSpark.HoverX <= plot.right)
        {
            pen = CreatePen(PS_DOT, 1, colors.Dim);
            oldPen = SelectObject(Dc, pen);
            MoveToEx(Dc, TrsSpark.HoverX, plot.top, NULL);
            LineTo(Dc, TrsSpark.HoverX, plot.bottom);
            SelectObject(Dc, oldPen);
            DeleteObject(pen);
        }

        // header text: name, value at hover or now, peak
        if (TrsSpark.HoverTime && hoverValue >= 0)
        {
            PPH_STRING when = TrsFormatLocal(TrsSpark.HoverTime, FALSE);

            header = PhFormatString(L"%s   %s: %.1f%%   peak %.1f%%", names[series], when->Buffer, hoverValue * 100, peak * 100);
            PhDereferenceObject(when);
        }
        else
        {
            header = PhFormatString(L"%s   now %.1f%%   peak %.1f%%", names[series], lastValue * 100, peak * 100);
        }

        textRect = strip;
        textRect.bottom = strip.top + headerHeight;
        SetTextColor(Dc, lineColors[series]);
        DrawText(Dc, header->Buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        PhDereferenceObject(header);

        if (series == 2)
        {
            PPH_STRING left = TrsFormatLocal(start, (end - start) > 20 * TR_FILETIME_HOUR);
            PPH_STRING right = TrsFormatLocal(end, (end - start) > 20 * TR_FILETIME_HOUR);
            PPH_STRING both = PhFormatString(L"%s  -  %s", left->Buffer, right->Buffer);

            SetTextColor(Dc, colors.Dim);
            DrawText(Dc, both->Buffer, -1, &textRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            PhDereferenceObject(both);
            PhDereferenceObject(right);
            PhDereferenceObject(left);
        }
    }

    if (oldFont)
        SelectObject(Dc, oldFont);
}

static LRESULT CALLBACK TrsSparkWndProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(WindowHandle, &ps);
            RECT client;
            HDC memoryDc;
            HBITMAP bitmap;
            HBITMAP oldBitmap;

            GetClientRect(WindowHandle, &client);
            memoryDc = CreateCompatibleDC(dc);
            bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
            oldBitmap = SelectObject(memoryDc, bitmap);
            TrsPaintSpark(WindowHandle, memoryDc);
            BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0, SRCCOPY);
            SelectObject(memoryDc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(memoryDc);
            EndPaint(WindowHandle, &ps);
        }
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
        {
            LONG x = GET_X_LPARAM(lParam);
            RECT client;
            LONG dpi = PhGetWindowDpi(WindowHandle);
            LONG pad = PhScaleToDisplay(8, dpi);
            LONGLONG start;
            LONGLONG end;
            LONG plotWidth;
            LONGLONG time;

            if (!TrsStoreGetSampleCount())
                return 0;

            GetClientRect(WindowHandle, &client);
            plotWidth = client.right - 2 * pad;

            if (plotWidth <= 0)
                return 0;

            TrsGetRange(&start, &end);
            x = max(pad, min(x, client.right - pad));
            time = start + (LONGLONG)(x - pad) * (end - start) / plotWidth;

            if (WindowMessage == WM_MOUSEMOVE)
            {
                TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, WindowHandle, 0 };

                TrackMouseEvent(&track);
                TrsSpark.HoverTime = time;
                TrsSpark.HoverX = x;
                InvalidateRect(WindowHandle, NULL, FALSE);
            }
            else
            {
                SetFocus(WindowHandle);
                SendMessage(GetParent(WindowHandle), TRS_WM_PICKED, 0, (LPARAM)&time);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        TrsSpark.HoverTime = 0;
        TrsSpark.HoverX = -1;
        InvalidateRect(WindowHandle, NULL, FALSE);
        return 0;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

// ---------------------------------------------------------------- container window

static VOID TrsUpdateFont(
    VOID
    )
{
    HFONT old = TrsFont;
    HWND controls[] = { TrsViewCombo, TrsRangeCombo, TrsDateControl, TrsTimeControl, TrsSpanCombo, TrsRankCombo, TrsLookupButton, TrsLatestButton, TrsInfoLabel };
    ULONG i;

    TrsFont = PhCreateApplicationFont(PhGetWindowDpi(TrsTabWindow));

    if (TrsFont)
    {
        for (i = 0; i < RTL_NUMBER_OF(controls); i++)
            SetWindowFont(controls[i], TrsFont, TRUE);
    }

    if (old)
        DeleteFont(old);
}

static VOID TrsLayout(
    VOID
    )
{
    RECT client;
    LONG dpi;
    LONG pad;
    LONG row;
    LONG width;
    LONG x;
    LONG top;
    LONG sparkHeight;
    LONG infoHeight;
    HDWP defer;

    if (!TrsCreated)
        return;

    PhGetClientRect(TrsTabWindow, &client);
    dpi = PhGetWindowDpi(TrsTabWindow);
    pad = PhScaleToDisplay(8, dpi);
    row = PhScaleToDisplay(26, dpi);
    width = client.right - client.left;
    sparkHeight = PhScaleToDisplay(190, dpi);
    infoHeight = PhScaleToDisplay(36, dpi);

    defer = BeginDeferWindowPos(12);
    top = pad;
    x = pad;
    defer = DeferWindowPos(defer, TrsViewCombo, NULL, x, top, PhScaleToDisplay(130, dpi), 300, SWP_NOZORDER | SWP_NOACTIVATE);
    x += PhScaleToDisplay(130, dpi) + pad;
    defer = DeferWindowPos(defer, TrsRangeCombo, NULL, x, top, PhScaleToDisplay(130, dpi), 300, SWP_NOZORDER | SWP_NOACTIVATE);

    top += row + pad / 2;
    x = pad;
    defer = DeferWindowPos(defer, TrsDateControl, NULL, x, top, PhScaleToDisplay(120, dpi), row - 2, SWP_NOZORDER | SWP_NOACTIVATE);
    x += PhScaleToDisplay(120, dpi) + pad;
    defer = DeferWindowPos(defer, TrsTimeControl, NULL, x, top, PhScaleToDisplay(100, dpi), row - 2, SWP_NOZORDER | SWP_NOACTIVATE);
    x += PhScaleToDisplay(100, dpi) + pad;
    defer = DeferWindowPos(defer, TrsSpanCombo, NULL, x, top, PhScaleToDisplay(110, dpi), 300, SWP_NOZORDER | SWP_NOACTIVATE);
    x += PhScaleToDisplay(110, dpi) + pad;
    defer = DeferWindowPos(defer, TrsRankCombo, NULL, x, top, PhScaleToDisplay(130, dpi), 300, SWP_NOZORDER | SWP_NOACTIVATE);
    x += PhScaleToDisplay(130, dpi) + pad;
    defer = DeferWindowPos(defer, TrsLookupButton, NULL, x, top, PhScaleToDisplay(80, dpi), row - 2, SWP_NOZORDER | SWP_NOACTIVATE);
    x += PhScaleToDisplay(80, dpi) + pad;
    defer = DeferWindowPos(defer, TrsLatestButton, NULL, x, top, PhScaleToDisplay(80, dpi), row - 2, SWP_NOZORDER | SWP_NOACTIVATE);

    top += row + pad;
    defer = DeferWindowPos(defer, TrsSparkWindow, NULL, 0, top, width, sparkHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    top += sparkHeight;
    defer = DeferWindowPos(defer, TrsInfoLabel, NULL, pad, top + pad / 2, max(width - 2 * pad, 0), infoHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    top += infoHeight + pad;
    defer = DeferWindowPos(defer, TrsUsageTable.Handle, NULL, 0, top, width, max(client.bottom - top, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    defer = DeferWindowPos(defer, TrsLeakTable.Handle, NULL, 0, top, width, max(client.bottom - top, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    EndDeferWindowPos(defer);
}

static VOID TrsApplyRankSort(
    VOID
    )
{
    ULONG rank = (ULONG)SendMessage(TrsRankCombo, CB_GETCURSEL, 0, 0);

    TreeNew_SetSort(TrsUsageTable.Handle, rank == 1 ? TRS_UC_PEAKPRIV : TRS_UC_AVGCPU, DescendingSortOrder);
}

static LRESULT CALLBACK TrsTabWndProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_SIZE:
        TrsLayout();
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
        {
            if (TrsUsageTable.Handle)
                SetFocus(TrsLeakViewActive() ? TrsLeakTable.Handle : TrsUsageTable.Handle);
        }
        return 0;
    case TRS_WM_PICKED:
        {
            TrsLookupCenter = *(PLONGLONG)lParam;
            TrsFollowLatest = FALSE;
            TrsSetControlsFromCenter();

            if (TrsLeakViewActive())
                SendMessage(TrsViewCombo, CB_SETCURSEL, 0, 0);

            TrsUpdateControlState();
            TrsRefreshAll();
        }
        return 0;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if ((header->hwndFrom == TrsDateControl || header->hwndFrom == TrsTimeControl) && header->code == DTN_DATETIMECHANGE && !TrsUpdatingControls)
            {
                TrsReadCenterFromControls();
                TrsRefreshAll();
            }
        }
        return 0;
    case WM_COMMAND:
        {
            ULONG id = GET_WM_COMMAND_ID(wParam, lParam);
            ULONG code = GET_WM_COMMAND_CMD(wParam, lParam);

            if (id == TRS_ID_VIEW && code == CBN_SELCHANGE)
            {
                PhSetIntegerSetting(SETTING_NAME_VIEW_INDEX, TrsLeakViewActive() ? 1 : 0);
                TrsUpdateControlState();
                TrsRefreshAll();
            }
            else if (id == TRS_ID_RANGE && code == CBN_SELCHANGE)
            {
                PhSetIntegerSetting(SETTING_NAME_RANGE_INDEX, (ULONG)SendMessage(TrsRangeCombo, CB_GETCURSEL, 0, 0));
                InvalidateRect(TrsSparkWindow, NULL, FALSE);
            }
            else if ((id == TRS_ID_SPAN || id == TRS_ID_RANK) && code == CBN_SELCHANGE)
            {
                if (id == TRS_ID_RANK)
                    TrsApplyRankSort();

                TrsRefreshAll();
            }
            else if (id == TRS_ID_LOOKUP && code == BN_CLICKED)
            {
                TrsReadCenterFromControls();
                TrsRefreshAll();
            }
            else if (id == TRS_ID_LATEST && code == BN_CLICKED)
            {
                TrsFollowLatest = TRUE;
                TrsRefreshAll();
            }
        }
        return 0;
    case WM_DESTROY:
        {
            if (TrsFont)
            {
                DeleteFont(TrsFont);
                TrsFont = NULL;
            }
        }
        break;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

static BOOLEAN TrsRegisterClasses(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static BOOLEAN ok = FALSE;

    if (PhBeginInitOnce(&initOnce))
    {
        WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_DATE_CLASSES | ICC_STANDARD_CLASSES };

        InitCommonControlsEx(&icc);

        wcex.lpfnWndProc = TrsTabWndProc;
        wcex.hInstance = PluginInstance->DllBase;
        wcex.hCursor = PhLoadCursor(NULL, IDC_ARROW);
        wcex.lpszClassName = TRS_TAB_CLASS;
        ok = RegisterClassEx(&wcex) != 0;

        wcex.lpfnWndProc = TrsSparkWndProc;
        wcex.lpszClassName = TRS_SPARK_CLASS;
        ok = RegisterClassEx(&wcex) != 0 && ok;

        PhEndInitOnce(&initOnce);
    }

    return ok;
}

static HWND TrsCreateControl(
    _In_ PCWSTR ClassName,
    _In_ PCWSTR Text,
    _In_ ULONG Style,
    _In_ ULONG Id
    )
{
    return CreateWindowEx(0, ClassName, Text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | Style, 0, 0, 0, 0, TrsTabWindow, (HMENU)(ULONG_PTR)Id, PluginInstance->DllBase, NULL);
}

static VOID TrsFillCombo(
    _In_ HWND Combo,
    _In_ PCWSTR *Items,
    _In_ ULONG Count,
    _In_ ULONG Selected
    )
{
    ULONG i;

    for (i = 0; i < Count; i++)
        ComboBox_AddString(Combo, Items[i]);

    ComboBox_SetCurSel(Combo, Selected);
}

static BOOLEAN TrsCreateTabWindow(
    _In_ HWND Parent
    )
{
    RECT client = { 0 };
    BOOLEAN themeSupport = !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT);
    static PCWSTR views[] = { L"Usage at time", L"Leak suspects" };
    static PCWSTR ranges[] = { L"Last hour", L"Last 6 hours", L"Last 24 hours", L"Last 7 days", L"All recorded" };
    static PCWSTR spans[] = { L"\x00B1 1 minute", L"\x00B1 5 minutes", L"\x00B1 15 minutes", L"\x00B1 1 hour" };
    static PCWSTR ranks[] = { L"Rank by CPU", L"Rank by memory" };
    ULONG rangeIndex;

    if (!TrsRegisterClasses())
        return FALSE;

    PhGetClientRect(Parent, &client);
    TrsTabWindow = PhCreateWindow(
        TRS_TAB_CLASS,
        NULL,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        client.left, client.top, client.right - client.left, client.bottom - client.top,
        Parent,
        NULL,
        PluginInstance->DllBase,
        NULL
        );

    if (!TrsTabWindow)
        return FALSE;

    TrsViewCombo = TrsCreateControl(WC_COMBOBOX, NULL, CBS_DROPDOWNLIST | WS_VSCROLL, TRS_ID_VIEW);
    TrsRangeCombo = TrsCreateControl(WC_COMBOBOX, NULL, CBS_DROPDOWNLIST | WS_VSCROLL, TRS_ID_RANGE);
    TrsDateControl = TrsCreateControl(DATETIMEPICK_CLASS, NULL, DTS_SHORTDATEFORMAT, TRS_ID_DATE);
    TrsTimeControl = TrsCreateControl(DATETIMEPICK_CLASS, NULL, DTS_TIMEFORMAT | DTS_UPDOWN, TRS_ID_TIME);
    TrsSpanCombo = TrsCreateControl(WC_COMBOBOX, NULL, CBS_DROPDOWNLIST | WS_VSCROLL, TRS_ID_SPAN);
    TrsRankCombo = TrsCreateControl(WC_COMBOBOX, NULL, CBS_DROPDOWNLIST | WS_VSCROLL, TRS_ID_RANK);
    TrsLookupButton = TrsCreateControl(WC_BUTTON, L"Look up", BS_PUSHBUTTON, TRS_ID_LOOKUP);
    TrsLatestButton = TrsCreateControl(WC_BUTTON, L"Latest", BS_PUSHBUTTON, TRS_ID_LATEST);
    TrsInfoLabel = CreateWindowEx(0, WC_STATIC, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, TrsTabWindow, (HMENU)(ULONG_PTR)TRS_ID_INFO, PluginInstance->DllBase, NULL);
    TrsSparkWindow = CreateWindowEx(0, TRS_SPARK_CLASS, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, TrsTabWindow, (HMENU)(ULONG_PTR)TRS_ID_SPARK, PluginInstance->DllBase, NULL);

    if (!TrsViewCombo || !TrsRangeCombo || !TrsDateControl || !TrsTimeControl || !TrsSpanCombo || !TrsRankCombo || !TrsLookupButton || !TrsLatestButton || !TrsInfoLabel || !TrsSparkWindow)
        return FALSE;

    rangeIndex = PhGetIntegerSetting(SETTING_NAME_RANGE_INDEX);

    if (rangeIndex >= RTL_NUMBER_OF(ranges))
        rangeIndex = 1;

    TrsFillCombo(TrsViewCombo, views, RTL_NUMBER_OF(views), PhGetIntegerSetting(SETTING_NAME_VIEW_INDEX) == 1 ? 1 : 0);
    TrsFillCombo(TrsRangeCombo, ranges, RTL_NUMBER_OF(ranges), rangeIndex);
    TrsFillCombo(TrsSpanCombo, spans, RTL_NUMBER_OF(spans), 1);
    TrsFillCombo(TrsRankCombo, ranks, RTL_NUMBER_OF(ranks), 0);
    DateTime_SetFormat(TrsTimeControl, L"HH':'mm':'ss");

    // tables
    TrsInitializeTable(&TrsUsageTable, TrsTabWindow, SETTING_NAME_USAGE_COLUMNS, SETTING_NAME_USAGE_SORT, &TrsUsageEmptyText, TRS_UC_AVGCPU);
    TrsInitializeTable(&TrsLeakTable, TrsTabWindow, SETTING_NAME_LEAK_COLUMNS, SETTING_NAME_LEAK_SORT, &TrsLeakEmptyText, TRS_LC_GROWTH);

    if (!TrsUsageTable.Handle || !TrsLeakTable.Handle)
        return FALSE;

    TrsUsageTable.ColumnCount = 9;
    TrsUsageTable.TextColumn[TRS_UC_NAME] = TRUE;
    TrsUsageTable.TextColumn[TRS_UC_STATUS] = FALSE;
    TrsLeakTable.ColumnCount = 7;
    TrsLeakTable.TextColumn[TRS_LC_NAME] = TRUE;

    TreeNew_SetRedraw(TrsUsageTable.Handle, FALSE);
    PhAddTreeNewColumn(TrsUsageTable.Handle, TRS_UC_NAME, TRUE, L"Process", 180, PH_ALIGN_LEFT, TRS_UC_NAME, 0);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_PID, TRUE, L"PID", 60, PH_ALIGN_RIGHT, TRS_UC_PID, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_AVGCPU, TRUE, L"Avg CPU", 80, PH_ALIGN_RIGHT, TRS_UC_AVGCPU, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_PEAKCPU, TRUE, L"Peak CPU", 80, PH_ALIGN_RIGHT, TRS_UC_PEAKCPU, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_AVGPRIV, TRUE, L"Avg private bytes", 110, PH_ALIGN_RIGHT, TRS_UC_AVGPRIV, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_PEAKPRIV, TRUE, L"Peak private bytes", 120, PH_ALIGN_RIGHT, TRS_UC_PEAKPRIV, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_PEAKWS, TRUE, L"Peak working set", 110, PH_ALIGN_RIGHT, TRS_UC_PEAKWS, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsUsageTable.Handle, TRS_UC_SAMPLES, TRUE, L"Samples", 70, PH_ALIGN_RIGHT, TRS_UC_SAMPLES, DT_RIGHT, TRUE);
    PhAddTreeNewColumn(TrsUsageTable.Handle, TRS_UC_STATUS, TRUE, L"Now", 70, PH_ALIGN_LEFT, TRS_UC_STATUS, 0);
    TrsFinishTable(&TrsUsageTable, &TrsUsageEmptyText);
    TreeNew_SetRedraw(TrsUsageTable.Handle, TRUE);

    TreeNew_SetRedraw(TrsLeakTable.Handle, FALSE);
    PhAddTreeNewColumn(TrsLeakTable.Handle, TRS_LC_NAME, TRUE, L"Process", 200, PH_ALIGN_LEFT, TRS_LC_NAME, 0);
    PhAddTreeNewColumnEx(TrsLeakTable.Handle, TRS_LC_PID, TRUE, L"PID", 60, PH_ALIGN_RIGHT, TRS_LC_PID, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsLeakTable.Handle, TRS_LC_CURRENT, TRUE, L"Private bytes", 110, PH_ALIGN_RIGHT, TRS_LC_CURRENT, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsLeakTable.Handle, TRS_LC_GROWTH, TRUE, L"Growth", 110, PH_ALIGN_RIGHT, TRS_LC_GROWTH, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsLeakTable.Handle, TRS_LC_RATE, TRUE, L"Rate", 110, PH_ALIGN_RIGHT, TRS_LC_RATE, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsLeakTable.Handle, TRS_LC_WINDOW, TRUE, L"Observed", 80, PH_ALIGN_RIGHT, TRS_LC_WINDOW, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(TrsLeakTable.Handle, TRS_LC_STEADY, TRUE, L"Steady", 70, PH_ALIGN_RIGHT, TRS_LC_STEADY, DT_RIGHT, TRUE);
    TrsFinishTable(&TrsLeakTable, &TrsLeakEmptyText);
    TreeNew_SetRedraw(TrsLeakTable.Handle, TRUE);

    if (themeSupport)
        PhInitializeWindowTheme(TrsTabWindow, TRUE);

    TrsUpdateFont();
    TrsCreated = TRUE;
    TrsUpdateControlState();
    TrsLayout();
    TrsRefreshAll();
    return TRUE;
}

// ---------------------------------------------------------------- tab page

_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
static BOOLEAN NTAPI TrsTabPageCallback(
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
            if (!TrsCreateTabWindow(Parameter2))
                return FALSE;

            if (Parameter1)
                *(HWND *)Parameter1 = TrsTabWindow;
        }
        return TRUE;
    case MainTabPageLoadSettings:
        return TRUE;
    case MainTabPageSaveSettings:
        {
            if (TrsCreated)
            {
                TrsSaveTable(&TrsUsageTable);
                TrsSaveTable(&TrsLeakTable);
            }
        }
        return TRUE;
    case MainTabPageSelected:
        {
            if (Parameter1)
                TrsRefreshAll();
        }
        break;
    case MainTabPageFontChanged:
        {
            if (TrsCreated)
            {
                SendMessage(TrsUsageTable.Handle, WM_SETFONT, (WPARAM)Parameter1, TRUE);
                SendMessage(TrsLeakTable.Handle, WM_SETFONT, (WPARAM)Parameter1, TRUE);
            }
        }
        break;
    case MainTabPageDpiChanged:
        {
            if (TrsCreated)
            {
                TrsUpdateFont();
                TrsLayout();
            }
        }
        break;
    }

    return FALSE;
}

VOID TrsSelectTab(
    VOID
    )
{
    if (TrsTabPage)
        SystemInformer_SelectTabPage(TrsTabPage->Index);
}

VOID TrsInitializeTab(
    VOID
    )
{
    PH_MAIN_TAB_PAGE page;

    memset(&page, 0, sizeof(PH_MAIN_TAB_PAGE));
    page.Name = TrsTabPageText;
    page.Callback = TrsTabPageCallback;
    TrsTabPage = PhPluginCreateTabPage(&page);
}
