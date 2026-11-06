/*
 * TaskManagerPlus: process list grouping and totals row.
 *
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * The Processes tab can show two header rows, "Apps (n)" and "Background processes (n)".
 * A process is an App when it owns a visible top-level window; everything else is Background.
 * The parent/child tree is kept inside each group: a process whose parent lives in the other
 * group becomes a root of its own group. The real PH_PROCESS_NODE.Parent/Children links are never
 * touched, only the lists returned to the tree control, so turning grouping off restores the
 * original behaviour exactly.
 *
 * The header rows are PH_PROCESS_NODEs with a NULL ProcessItem. They are not in the process node
 * list, so selection and command code never sees them.
 *
 * This file also supplies the text of the totals row shown under the column headers.
 */

#include <phapp.h>
#include <proctree.h>
#include <procprv.h>
#include <procgroup.h>
#include <settings.h>

static HWND GroupTreeHandle;
static BOOLEAN GroupEnabled = FALSE;
static PH_PROCESS_NODE GroupHeaderNodes[PH_PROCESS_GROUP_COUNT];
static PPH_TREENEW_NODE GroupHeaderNodePointers[PH_PROCESS_GROUP_COUNT];
static PPH_LIST GroupRoots[PH_PROCESS_GROUP_COUNT]; // process nodes whose tree parent is not in the group
static PPH_LIST GroupAll[PH_PROCESS_GROUP_COUNT]; // every process node in the group (flat sorted view)
static WCHAR GroupHeaderText[PH_PROCESS_GROUP_COUNT][64];
static ULONG GroupVisibleCount[PH_PROCESS_GROUP_COUNT];

// Snapshot of process IDs that own a visible top-level window (sorted, rebuilt each refresh).
static PULONG WindowPids;
static ULONG WindowPidCount;
static ULONG WindowPidCapacity;
static BOOLEAN WindowSnapshotValid;

static WCHAR TotalsCpuText[32];
static WCHAR TotalsMemoryText[32];
static WCHAR TotalsDiskText[48];

static VOID PhpGroupAddWindowPid(
    _In_ ULONG ProcessId
    )
{
    if (WindowPidCount == WindowPidCapacity)
    {
        ULONG capacity = WindowPidCapacity ? WindowPidCapacity * 2 : 128;
        PULONG pids = PhAllocate(capacity * sizeof(ULONG));

        if (WindowPids)
        {
            memcpy(pids, WindowPids, WindowPidCount * sizeof(ULONG));
            PhFree(WindowPids);
        }

        WindowPids = pids;
        WindowPidCapacity = capacity;
    }

    WindowPids[WindowPidCount++] = ProcessId;
}

static int __cdecl PhpGroupPidCompare(
    _In_ const void *Elem1,
    _In_ const void *Elem2
    )
{
    ULONG a = *(PULONG)Elem1;
    ULONG b = *(PULONG)Elem2;

    return a < b ? -1 : (a > b ? 1 : 0);
}

static BOOL CALLBACK PhpGroupEnumWindowsProc(
    _In_ HWND WindowHandle,
    _In_ LPARAM Context
    )
{
    ULONG processId;
    ULONG cloaked;
    LONG_PTR exStyle;

    UNREFERENCED_PARAMETER(Context);

    if (!IsWindowVisible(WindowHandle))
        return TRUE;
    if (GetWindow(WindowHandle, GW_OWNER))
        return TRUE;

    exStyle = GetWindowLongPtr(WindowHandle, GWL_EXSTYLE);

    if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW))
        return TRUE;
    if (GetWindowTextLength(WindowHandle) == 0)
        return TRUE;

    // Windows of suspended / hidden UWP apps are cloaked even though they are "visible".
    cloaked = 0;
    if (SUCCEEDED(PhGetWindowThemeAttribute(WindowHandle, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;

    if (GetWindowThreadProcessId(WindowHandle, &processId) && processId)
        PhpGroupAddWindowPid(processId);

    return TRUE;
}

static VOID PhpGroupUpdateWindowSnapshot(
    VOID
    )
{
    WindowPidCount = 0;
    EnumWindows(PhpGroupEnumWindowsProc, 0);

    if (WindowPidCount > 1)
        qsort(WindowPids, WindowPidCount, sizeof(ULONG), PhpGroupPidCompare);

    WindowSnapshotValid = TRUE;
}

static BOOLEAN PhpGroupProcessHasWindow(
    _In_ HANDLE ProcessId
    )
{
    ULONG pid = HandleToUlong(ProcessId);

    if (!WindowPidCount)
        return FALSE;

    return bsearch(&pid, WindowPids, WindowPidCount, sizeof(ULONG), PhpGroupPidCompare) != NULL;
}

VOID PhGroupInitialization(
    _In_ HWND TreeNewHandle
    )
{
    ULONG i;

    GroupTreeHandle = TreeNewHandle;
    GroupEnabled = !!PhGetIntegerSetting(SETTING_GROUP_PROCESSES);

    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        if (!GroupRoots[i])
        {
            memset(&GroupHeaderNodes[i], 0, sizeof(PH_PROCESS_NODE));
            PhInitializeTreeNewNode(&GroupHeaderNodes[i].Node);
            GroupHeaderNodes[i].Node.Visible = FALSE;
            GroupHeaderNodePointers[i] = &GroupHeaderNodes[i].Node;
            GroupRoots[i] = PhCreateList(32);
            GroupAll[i] = PhCreateList(64);
        }
    }
}

BOOLEAN PhGroupEnabled(
    VOID
    )
{
    return GroupEnabled;
}

BOOLEAN PhGroupIsHeaderNode(
    _In_opt_ PVOID Node
    )
{
    return Node && (Node == &GroupHeaderNodes[0] || Node == &GroupHeaderNodes[1]);
}

VOID PhGroupSetEnabled(
    _In_ BOOLEAN Enabled,
    _In_ PPH_LIST NodeList
    )
{
    PhSetIntegerSetting(SETTING_GROUP_PROCESSES, Enabled);
    GroupEnabled = Enabled;

    if (Enabled)
        PhGroupRebuild(NodeList);

    TreeNew_NodesStructured(GroupTreeHandle);
}

BOOLEAN PhGroupRebuild(
    _In_ PPH_LIST NodeList
    )
{
    ULONG i;
    BOOLEAN changed = FALSE;

    if (!GroupEnabled || !GroupRoots[0])
        return FALSE;

    if (!WindowSnapshotValid)
        PhpGroupUpdateWindowSnapshot();

    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        PhClearList(GroupRoots[i]);
        PhClearList(GroupAll[i]);
        GroupVisibleCount[i] = 0;
    }

    for (i = 0; i < NodeList->Count; i++)
    {
        PPH_PROCESS_NODE node = NodeList->Items[i];
        ULONG group = PhpGroupProcessHasWindow(node->ProcessId) ? PH_PROCESS_GROUP_APPS : PH_PROCESS_GROUP_BACKGROUND;

        if (node->GroupId != group)
            changed = TRUE;

        node->GroupId = group;

        if (!node->GroupChildren)
            node->GroupChildren = PhCreateList(1);
        else
            PhClearList(node->GroupChildren);
    }

    for (i = 0; i < NodeList->Count; i++)
    {
        PPH_PROCESS_NODE node = NodeList->Items[i];

        PhAddItemList(GroupAll[node->GroupId], node);

        if (node->Node.Visible)
            GroupVisibleCount[node->GroupId]++;

        if (node->Parent && node->Parent->GroupId == node->GroupId && node->Parent->GroupChildren)
            PhAddItemList(node->Parent->GroupChildren, node);
        else
            PhAddItemList(GroupRoots[node->GroupId], node);
    }

    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        PH_FORMAT format[3];
        SIZE_T returnLength;

        PhInitFormatS(&format[0], i == PH_PROCESS_GROUP_APPS ? L"Apps (" : L"Background processes (");
        PhInitFormatU(&format[1], GroupVisibleCount[i]);
        PhInitFormatC(&format[2], L')');

        GroupHeaderText[i][0] = UNICODE_NULL;
        PhFormatToBuffer(format, 3, GroupHeaderText[i], sizeof(GroupHeaderText[i]), &returnLength);

        GroupHeaderNodes[i].Node.Visible = GroupVisibleCount[i] != 0;
    }

    return changed;
}

VOID PhGroupTick(
    _In_ PPH_LIST NodeList
    )
{
    RECT rect;

    if (!GroupTreeHandle)
        return;

    if (GroupEnabled)
    {
        PhpGroupUpdateWindowSnapshot();

        if (PhGroupRebuild(NodeList))
            TreeNew_NodesStructured(GroupTreeHandle);
    }

    // The totals row sits above the rows the tree repaints on a tick.
    if (PhGetClientRect(GroupTreeHandle, &rect))
    {
        PH_TREENEW_VIEW_PARTS viewParts;

        TreeNew_GetViewParts(GroupTreeHandle, &viewParts);
        rect.bottom = viewParts.HeaderHeight;
        InvalidateRect(GroupTreeHandle, &rect, FALSE);
    }
}

PPH_LIST PhGroupGetNodeChildren(
    _In_ PVOID Node
    )
{
    PPH_PROCESS_NODE node = Node;

    if (!GroupEnabled || PhGroupIsHeaderNode(node))
        return NULL;

    return node->GroupChildren;
}

BOOLEAN PhGroupGetChildren(
    _In_ PVOID Node,
    _Inout_ PPH_TREENEW_GET_CHILDREN GetChildren,
    _In_ BOOLEAN SortChildProcesses,
    _In_ BOOLEAN SortRootProcesses,
    _In_ PH_SORT_ORDER SortOrder,
    _Out_ PPH_LIST *SortList,
    _Inout_ PULONG SortColumn,
    _Inout_ PPH_SORT_ORDER SortColumnOrder,
    _In_ ULONG DefaultChildSortColumn
    )
{
    PPH_PROCESS_NODE node = Node;

    *SortList = NULL;

    if (!GroupEnabled)
        return FALSE;

    if (!node)
    {
        GetChildren->Children = GroupHeaderNodePointers;
        GetChildren->NumberOfChildren = PH_PROCESS_GROUP_COUNT;
        return TRUE;
    }

    if (PhGroupIsHeaderNode(node))
    {
        ULONG group = (ULONG)(node - GroupHeaderNodes);

        if (SortChildProcesses || SortOrder == NoSortOrder)
        {
            GetChildren->Children = (PPH_TREENEW_NODE *)GroupRoots[group]->Items;
            GetChildren->NumberOfChildren = GroupRoots[group]->Count;

            if (SortRootProcesses)
                *SortList = GroupRoots[group];
        }
        else
        {
            // Sorted flat view: all of the group's processes, no hierarchy.
            GetChildren->Children = (PPH_TREENEW_NODE *)GroupAll[group]->Items;
            GetChildren->NumberOfChildren = GroupAll[group]->Count;
            *SortList = GroupAll[group];
        }

        return TRUE;
    }

    if (!node->GroupChildren)
        return TRUE; // no children yet

    if (SortChildProcesses)
    {
        GetChildren->Children = (PPH_TREENEW_NODE *)node->GroupChildren->Items;
        GetChildren->NumberOfChildren = node->GroupChildren->Count;
        *SortList = node->GroupChildren;

        if (SortOrder == NoSortOrder)
        {
            *SortColumn = DefaultChildSortColumn;
            *SortColumnOrder = AscendingSortOrder;
        }
    }
    else if (SortOrder == NoSortOrder)
    {
        GetChildren->Children = (PPH_TREENEW_NODE *)node->GroupChildren->Items;
        GetChildren->NumberOfChildren = node->GroupChildren->Count;
    }

    return TRUE;
}

BOOLEAN PhGroupExpandAll(
    _In_ BOOLEAN Expand
    )
{
    ULONG i;
    BOOLEAN changed = FALSE;

    if (!GroupEnabled)
        return FALSE;

    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        if (GroupHeaderNodes[i].Node.Expanded != Expand)
        {
            GroupHeaderNodes[i].Node.Expanded = Expand;
            changed = TRUE;
        }
    }

    return changed;
}

static BOOLEAN PhpGroupGetTotalsText(
    _Inout_ PPH_TREENEW_GET_TOTALS_TEXT Totals
    )
{
    PH_FORMAT format[3];
    SIZE_T returnLength;
    PWCHAR buffer;
    ULONG bufferSize;
    ULONG count;

    switch (Totals->ColumnId)
    {
    case PHPRTLC_CPU:
        {
            FLOAT usage = (PhCpuKernelUsage + PhCpuUserUsage) * 100;

            if (usage > 100)
                usage = 100;

            PhInitFormatF(&format[0], usage, 0);
            PhInitFormatC(&format[1], L'%');
            count = 2;
            buffer = TotalsCpuText;
            bufferSize = sizeof(TotalsCpuText);
        }
        break;
    case PHPRTLC_PRIVATEBYTES:
        {
            // The memory column: physical memory in use, as a share of installed memory.
            ULONG_PTR total = PhSystemBasicInformation.NumberOfPhysicalPages;
            ULONG_PTR used;

            if (!total)
                return FALSE;

            used = total - min(total, PhPerfInformation.AvailablePages);

            PhInitFormatF(&format[0], (FLOAT)used * 100 / (FLOAT)total, 0);
            PhInitFormatC(&format[1], L'%');
            count = 2;
            buffer = TotalsMemoryText;
            bufferSize = sizeof(TotalsMemoryText);
        }
        break;
    case PHPRTLC_IOTOTALRATE:
        {
            ULONG64 rate;

            if (!PhProcessProviderElapsedMilliseconds)
                return FALSE;

            rate = PhIoReadDelta.Delta + PhIoWriteDelta.Delta + PhIoOtherDelta.Delta;
            rate = rate * 1000 / PhProcessProviderElapsedMilliseconds;

            PhInitFormatSizeWithPrecision(&format[0], rate, 1); // short, e.g. 321.3 kB/s
            PhInitFormatS(&format[1], L"/s");
            count = 2;
            buffer = TotalsDiskText;
            bufferSize = sizeof(TotalsDiskText);
        }
        break;
    default:
        return FALSE;
    }

    buffer[0] = UNICODE_NULL;

    if (!PhFormatToBuffer(format, count, buffer, bufferSize, &returnLength))
        return FALSE;

    Totals->Text.Buffer = buffer;
    Totals->Text.Length = returnLength - sizeof(UNICODE_NULL);
    return TRUE;
}

BOOLEAN PhGroupTreeNewCallback(
    _In_ PH_TREENEW_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _Out_ PBOOLEAN Result
    )
{
    // Every per-node parameter block starts with { ULONG Flags; PPH_TREENEW_NODE Node; }.
    PPH_TREENEW_IS_LEAF nodeBlock = Parameter1;

    *Result = FALSE;

    if (Message == TreeNewGetTotalsText)
    {
        *Result = PhpGroupGetTotalsText(Parameter1);
        return TRUE;
    }

    *Result = TRUE;

    if (!GroupEnabled || !Parameter1)
        return FALSE;

    switch (Message)
    {
    case TreeNewIsLeaf:
        if (PhGroupIsHeaderNode(nodeBlock->Node))
        {
            ULONG group = (ULONG)((PPH_PROCESS_NODE)nodeBlock->Node - GroupHeaderNodes);

            nodeBlock->IsLeaf = GroupRoots[group]->Count == 0;
            *Result = TRUE;
            return TRUE;
        }
        break;
    case TreeNewGetCellText:
        if (PhGroupIsHeaderNode(nodeBlock->Node))
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;

            if (getCellText->Id == PHPRTLC_NAME)
            {
                ULONG group = (ULONG)((PPH_PROCESS_NODE)nodeBlock->Node - GroupHeaderNodes);

                PhInitializeStringRefLongHint(&getCellText->Text, GroupHeaderText[group]);
            }

            return TRUE;
        }
        break;
    case TreeNewGetNodeColor:
        if (PhGroupIsHeaderNode(nodeBlock->Node))
        {
            // TaskManagerPlus: flat group header, window background with muted text.
            PPH_TREENEW_GET_NODE_COLOR color = Parameter1;
            COLORREF back = PhEnableThemeSupport ? PhThemeWindowBackgroundColor : GetSysColor(COLOR_WINDOW);
            COLORREF fore = PhEnableThemeSupport ? PhThemeWindowTextColor : GetSysColor(COLOR_WINDOWTEXT);

            color->Flags = 0;
            color->BackColor = back;
            color->ForeColor = RGB(
                (GetRValue(fore) * 190 + GetRValue(back) * 65) / 255,
                (GetGValue(fore) * 190 + GetGValue(back) * 65) / 255,
                (GetBValue(fore) * 190 + GetBValue(back) * 65) / 255
                );
            *Result = TRUE;
            return TRUE;
        }
        break;
    case TreeNewGetNodeIcon:
    case TreeNewGetNodeFont:
    case TreeNewGetCellTooltip:
    case TreeNewCustomDraw:
        if (PhGroupIsHeaderNode(nodeBlock->Node))
        {
            *Result = FALSE; // defaults; the generic code would dereference the NULL process item
            return TRUE;
        }
        break;
    case TreeNewLeftDoubleClick:
    case TreeNewRightClick:
    case TreeNewMiddleClick:
    case TreeNewRightDoubleClick:
        {
            PPH_TREENEW_MOUSE_EVENT mouseEvent = Parameter1;

            if (PhGroupIsHeaderNode(mouseEvent->Node))
                return TRUE;
        }
        break;
    case TreeNewContextMenu:
        {
            PPH_TREENEW_CONTEXT_MENU contextMenu = Parameter1;

            if (PhGroupIsHeaderNode(contextMenu->Node))
                return TRUE;
        }
        break;
    }

    return FALSE;
}
