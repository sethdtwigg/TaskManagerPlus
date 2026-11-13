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
#include <heatmap.h> // TaskManagerPlus
#include <metricfmt.h> // TaskManagerPlus
#include <actions.h>
#include <mainwnd.h>
#include <emenu.h>
#include <phsettings.h>

// ---- Group by app ------------------------------------------------------------------------------------------
// App rows are fake PH_PROCESS_NODEs (NULL ProcessItem, GroupId = the magic below) that sit between a section
// header and the member processes. They live in AppRows, persist across refreshes (so selection and expansion
// survive) and are only shown while they have 2 or more members.

#define PH_APP_ROW_MAGIC 0x41505052

typedef struct _PH_APP_ROW
{
    PH_PROCESS_NODE Node; // must be first
    ULONG Hash; // section + key type + key, case-insensitive
    ULONG Section;
    WCHAR KeyType; // 'p' package family, 'f' image path, 'n' image name
    PPH_STRING Key;
    PPH_LIST Members;
    PPH_PROCESS_NODE Leader;
    ULONG VisibleCount;
    BOOLEAN Dead; // no members; freed on the next tick, after the tree has been restructured
    WCHAR Name[112];
    WCHAR Text[8][48];
    ULONG TextColumn[8];
} PH_APP_ROW, *PPH_APP_ROW;

static BOOLEAN GroupByAppEnabled;
static PPH_LIST AppRows;
static ULONG ExpandedHashes[128];
static ULONG ExpandedHashCount;
static PH_STRINGREF WindowsDirectory;
static BOOLEAN FreeDeadRows;

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

// ---- Group by app: helpers ---------------------------------------------------------------------------------

BOOLEAN PhGroupByApp(
    VOID
    )
{
    return GroupByAppEnabled;
}

BOOLEAN PhGroupIsAppNode(
    _In_opt_ PVOID Node
    )
{
    PPH_PROCESS_NODE node = Node;

    return node && node->ProcessItem == NULL && node->GroupId == PH_APP_ROW_MAGIC;
}

static ULONG PhpGroupHashStringRef(
    _In_ ULONG Seed,
    _In_ PPH_STRINGREF String
    )
{
    ULONG hash = Seed;

    for (SIZE_T i = 0; i < String->Length / sizeof(WCHAR); i++)
    {
        hash ^= (ULONG)RtlUpcaseUnicodeChar(String->Buffer[i]);
        hash *= 16777619;
    }

    return hash;
}

// The grouping key of a process: the package family, else the image path, else the image name.
static VOID PhpGroupGetKey(
    _In_ PPH_PROCESS_NODE Node,
    _Out_ PWCHAR Type,
    _Out_ PPH_STRINGREF Key
    )
{
    PPH_PROCESS_ITEM item = Node->ProcessItem;

    if (!PhIsNullOrEmptyString(item->PackageFullName))
    {
        SIZE_T index;

        *Type = L'p';
        *Key = item->PackageFullName->sr;
        index = PhFindCharInStringRef(Key, L'_', FALSE);

        if (index != SIZE_MAX)
            Key->Length = index * sizeof(WCHAR);
    }
    else if (!PhIsNullOrEmptyString(item->FileName))
    {
        *Type = L'f';
        *Key = item->FileName->sr;
    }
    else
    {
        *Type = L'n';
        *Key = item->ProcessName->sr;
    }
}

// Apps = owns a window; Windows processes = lives under the Windows directory (or is a pseudo process);
// everything else is a background process.
static ULONG PhpGroupGetAppSection(
    _In_ PPH_PROCESS_NODE Node
    )
{
    PPH_PROCESS_ITEM item = Node->ProcessItem;

    if (PhpGroupProcessHasWindow(Node->ProcessId))
        return PH_PROCESS_GROUP_APPS;

    if (!PH_IS_REAL_PROCESS_ID(item->ProcessId) || HandleToUlong(item->ProcessId) == 4)
        return PH_PROCESS_GROUP_WINDOWS;

    if (!PhIsNullOrEmptyString(item->FileName) && WindowsDirectory.Length > 2 * sizeof(WCHAR))
    {
        PH_STRINGREF path = item->FileName->sr;
        PH_STRINGREF tail; // "\WINDOWS", without the drive

        tail.Buffer = WindowsDirectory.Buffer + 2;
        tail.Length = WindowsDirectory.Length - 2 * sizeof(WCHAR);

        if (path.Length > 2 * sizeof(WCHAR) && path.Buffer[1] == L':')
        {
            path.Buffer += 2; // C:\WINDOWS\... -> \WINDOWS\...
            path.Length -= 2 * sizeof(WCHAR);
        }
        else if (PhStartsWithStringRef2(&path, L"\\Device\\", TRUE))
        {
            // \Device\HarddiskVolume4\Windows\... (protected processes report the NT path): skip the device name.
            PH_STRINGREF rest;

            path.Buffer += 8;
            path.Length -= 8 * sizeof(WCHAR);
            PhSplitStringRefAtChar(&path, L'\\', &rest, &path); // rest = "HarddiskVolume4", path = "Windows\..."

            path.Buffer--; // keep the separator: "\Windows\..."
            path.Length += sizeof(WCHAR);
        }

        if (path.Length > tail.Length + sizeof(WCHAR) && path.Buffer[tail.Length / sizeof(WCHAR)] == L'\\')
        {
            PH_STRINGREF prefix;

            prefix.Buffer = path.Buffer;
            prefix.Length = tail.Length;

            if (PhEqualStringRef(&prefix, &tail, TRUE))
                return PH_PROCESS_GROUP_WINDOWS;
        }
    }

    return PH_PROCESS_GROUP_BACKGROUND;
}

static BOOLEAN PhpGroupIsExpandedHash(
    _In_ ULONG Hash
    )
{
    for (ULONG i = 0; i < ExpandedHashCount; i++)
    {
        if (ExpandedHashes[i] == Hash)
            return TRUE;
    }

    return FALSE;
}

static VOID PhpGroupLoadExpanded(
    VOID
    )
{
    PPH_STRING text = PhGetStringSetting(SETTING_GROUP_APPS_EXPANDED);
    PH_STRINGREF remaining = text->sr;

    ExpandedHashCount = 0;

    while (remaining.Length && ExpandedHashCount < RTL_NUMBER_OF(ExpandedHashes))
    {
        PH_STRINGREF part;
        ULONG64 value;

        PhSplitStringRefAtChar(&remaining, L',', &part, &remaining);

        if (part.Length && PhStringToInteger64(&part, 16, &value))
            ExpandedHashes[ExpandedHashCount++] = (ULONG)value;
    }

    PhDereferenceObject(text);
}

// Persists the expanded app rows: the expanded rows now, plus remembered rows whose app is not running.
static VOID PhpGroupSaveExpanded(
    VOID
    )
{
    ULONG hashes[RTL_NUMBER_OF(ExpandedHashes)];
    ULONG count = 0;

    if (!AppRows)
        return;

    for (ULONG i = 0; i < AppRows->Count && count < RTL_NUMBER_OF(hashes); i++)
    {
        PPH_APP_ROW row = AppRows->Items[i];

        if (!row->Dead && row->Members->Count >= 2 && row->Node.Node.Expanded)
            hashes[count++] = row->Hash;
    }

    for (ULONG i = 0; i < ExpandedHashCount && count < RTL_NUMBER_OF(hashes); i++)
    {
        BOOLEAN present = FALSE;

        for (ULONG j = 0; j < AppRows->Count; j++)
        {
            if (((PPH_APP_ROW)AppRows->Items[j])->Hash == ExpandedHashes[i])
            {
                present = TRUE;
                break;
            }
        }

        if (!present)
            hashes[count++] = ExpandedHashes[i];
    }

    if (count == ExpandedHashCount && (!count || memcmp(hashes, ExpandedHashes, count * sizeof(ULONG)) == 0))
        return;

    memcpy(ExpandedHashes, hashes, count * sizeof(ULONG));
    ExpandedHashCount = count;

    {
        PH_STRING_BUILDER sb;

        PhInitializeStringBuilder(&sb, 64);

        for (ULONG i = 0; i < count; i++)
        {
            WCHAR buffer[16];

            swprintf_s(buffer, RTL_NUMBER_OF(buffer), L"%x,", hashes[i]);
            PhAppendStringBuilder2(&sb, buffer);
        }

        PhSetStringSetting2(SETTING_GROUP_APPS_EXPANDED, &sb.String->sr);
        PhDeleteStringBuilder(&sb);
    }
}

static PPH_APP_ROW PhpGroupFindOrCreateRow(
    _In_ WCHAR KeyType,
    _In_ PPH_STRINGREF Key
    )
{
    ULONG hash = PhpGroupHashStringRef(2166136261u ^ KeyType, Key);
    PPH_APP_ROW row;

    for (ULONG i = 0; i < AppRows->Count; i++)
    {
        row = AppRows->Items[i];

        if (row->Hash == hash && row->KeyType == KeyType &&
            PhEqualStringRef(&row->Key->sr, Key, TRUE))
            return row;
    }

    row = PhAllocateZero(sizeof(PH_APP_ROW));
    PhInitializeTreeNewNode(&row->Node.Node);
    row->Node.GroupId = PH_APP_ROW_MAGIC;
    row->Node.Node.Visible = FALSE;
    row->Node.Node.Expanded = PhpGroupIsExpandedHash(hash);
    row->Hash = hash;
    row->KeyType = KeyType;
    row->Key = PhCreateString2(Key);
    row->Members = PhCreateList(8);
    PhAddItemList(AppRows, row);

    return row;
}

static VOID PhpGroupFreeRow(
    _In_ PPH_APP_ROW Row
    )
{
    PhDereferenceObject(Row->Key);
    PhDereferenceObject(Row->Members);
    PhFree(Row);
}

// Marks every app row dead and detaches the processes (Group by app turned off).
static VOID PhpGroupDetachAll(
    _In_ PPH_LIST NodeList
    )
{
    if (!AppRows)
        return;

    for (ULONG i = 0; i < NodeList->Count; i++)
        ((PPH_PROCESS_NODE)NodeList->Items[i])->AppRow = NULL;

    for (ULONG i = 0; i < AppRows->Count; i++)
    {
        PPH_APP_ROW row = AppRows->Items[i];

        PhClearList(row->Members);
        row->Dead = TRUE;
        row->Node.Node.Visible = FALSE;
    }
}

static PH_STRINGREF PhpGroupGetProcessName(
    _In_ PPH_PROCESS_NODE Node
    )
{
    PPH_PROCESS_ITEM item = Node->ProcessItem;

    if (PhFriendlyNames && !PhIsNullOrEmptyString(item->VersionInfo.FileDescription))
        return item->VersionInfo.FileDescription->sr;

    return item->ProcessName->sr;
}

// Builds the section lists with app rows: rows with 2+ members stand in for their members.
static BOOLEAN PhpGroupRebuildApps(
    _In_ PPH_LIST NodeList
    )
{
    BOOLEAN changed = FALSE;
    ULONG i;

    // Rows emptied on an earlier tick have been taken out of the tree by now.
    if (FreeDeadRows)
    {
        for (i = AppRows->Count; i > 0; i--)
        {
            PPH_APP_ROW row = AppRows->Items[i - 1];

            if (row->Dead && row->Members->Count == 0)
            {
                PhRemoveItemList(AppRows, i - 1);
                PhpGroupFreeRow(row);
            }
        }
    }

    for (i = 0; i < AppRows->Count; i++)
        PhClearList(((PPH_APP_ROW)AppRows->Items[i])->Members);

    for (i = 0; i < NodeList->Count; i++)
    {
        PPH_PROCESS_NODE node = NodeList->Items[i];
        PPH_APP_ROW row;
        PH_STRINGREF key;
        WCHAR type;

        if (!node->GroupChildren)
            node->GroupChildren = PhCreateList(1);
        else
            PhClearList(node->GroupChildren);

        PhpGroupGetKey(node, &type, &key);
        row = PhpGroupFindOrCreateRow(type, &key);
        row->Dead = FALSE;
        PhAddItemList(row->Members, node);
        node->AppRow = NULL;
    }

    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        PhClearList(GroupRoots[i]);
        PhClearList(GroupAll[i]);
        GroupVisibleCount[i] = 0;
    }

    for (i = 0; i < AppRows->Count; i++)
    {
        PPH_APP_ROW row = AppRows->Items[i];
        PPH_PROCESS_NODE leader = NULL;
        ULONG visible = 0;
        BOOLEAN wasVisible = !!row->Node.Node.Visible;

        if (row->Members->Count == 0)
        {
            if (!row->Dead)
                row->Dead = TRUE; // freed on a later tick

            if (wasVisible)
                changed = TRUE;

            row->Node.Node.Visible = FALSE;
            continue;
        }

        for (ULONG j = 0; j < row->Members->Count; j++)
        {
            PPH_PROCESS_NODE member = row->Members->Items[j];
            BOOLEAN memberWindow = PhpGroupProcessHasWindow(member->ProcessId);

            if (member->Node.Visible)
                visible++;

            if (!leader)
            {
                leader = member;
            }
            else
            {
                BOOLEAN leaderWindow = PhpGroupProcessHasWindow(leader->ProcessId);

                if ((memberWindow && !leaderWindow) ||
                    (memberWindow == leaderWindow && member->ProcessItem->CreateTime.QuadPart < leader->ProcessItem->CreateTime.QuadPart))
                    leader = member;
            }
        }

        // The whole app lives in one section: Apps when any member owns a window, else by where it is installed.
        {
            ULONG section = PH_PROCESS_GROUP_BACKGROUND;
            BOOLEAN anyWindow = FALSE;

            for (ULONG j = 0; j < row->Members->Count && !anyWindow; j++)
                anyWindow = PhpGroupProcessHasWindow(((PPH_PROCESS_NODE)row->Members->Items[j])->ProcessId);

            section = anyWindow ? PH_PROCESS_GROUP_APPS : PhpGroupGetAppSection(row->Members->Items[0]);
            row->Section = section;

            for (ULONG j = 0; j < row->Members->Count; j++)
            {
                PPH_PROCESS_NODE member = row->Members->Items[j];

                if (member->GroupId != section)
                    changed = TRUE;

                member->GroupId = section;
            }
        }

        GroupVisibleCount[row->Section] += visible;
        row->Leader = leader;

        if (row->Members->Count >= 2)
        {
            PH_FORMAT format[4];
            SIZE_T returnLength;

            for (ULONG j = 0; j < row->Members->Count; j++)
                ((PPH_PROCESS_NODE)row->Members->Items[j])->AppRow = &row->Node;

            PhInitFormatSR(&format[0], PhpGroupGetProcessName(leader));
            PhInitFormatS(&format[1], L" (");
            PhInitFormatU(&format[2], visible);
            PhInitFormatC(&format[3], L')');
            row->Name[0] = UNICODE_NULL;
            PhFormatToBuffer(format, 4, row->Name, sizeof(row->Name), &returnLength);

            if (wasVisible != (visible != 0))
                changed = TRUE;

            row->VisibleCount = visible;
            row->Node.Node.Visible = visible != 0;
            PhAddItemList(GroupRoots[row->Section], row);
        }
        else
        {
            if (wasVisible)
                changed = TRUE;

            row->Node.Node.Visible = FALSE;
            PhAddItemList(GroupRoots[row->Section], row->Members->Items[0]);
        }
    }

    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
        GroupHeaderNodes[i].Node.Visible = GroupVisibleCount[i] != 0;

    return changed;
}

// Sorting of a section's root list: app rows sort against single processes by their combined numbers.
typedef struct _PH_GROUP_SORT_ITEM
{
    PVOID Node;
    DOUBLE Value;
    PH_STRINGREF Name;
} PH_GROUP_SORT_ITEM, *PPH_GROUP_SORT_ITEM;

static PH_SORT_ORDER GroupSortOrder;
static BOOLEAN GroupSortByName;

static int __cdecl PhpGroupSortCompare(
    _In_ const void *Elem1,
    _In_ const void *Elem2
    )
{
    const PH_GROUP_SORT_ITEM *a = Elem1;
    const PH_GROUP_SORT_ITEM *b = Elem2;
    int result;

    if (GroupSortByName)
    {
        result = PhCompareStringRef(&a->Name, &b->Name, TRUE);
        return GroupSortOrder == DescendingSortOrder ? -result : result;
    }

    result = a->Value < b->Value ? -1 : (a->Value > b->Value ? 1 : 0);

    if (GroupSortOrder == DescendingSortOrder)
        result = -result;

    if (result == 0)
        result = PhCompareStringRef(&a->Name, &b->Name, TRUE);

    return result;
}

BOOLEAN PhGroupGetAppMetric(
    _In_ PVOID Node,
    _In_ PPH_TREENEW_COLUMN Column,
    _Out_ PULONG Kind,
    _Out_ PDOUBLE Value
    )
{
    PPH_APP_ROW row = Node;

    *Kind = PH_HEAT_KIND_NONE;
    *Value = 0;

    for (ULONG i = 0; i < row->Members->Count; i++)
    {
        ULONG kind;
        DOUBLE value;

        if (PhGetProcessNodeMetric(row->Members->Items[i], Column, &kind, &value))
        {
            *Kind = kind;
            *Value += value;
        }
    }

    return *Kind != PH_HEAT_KIND_NONE;
}

static VOID PhpGroupSortRoots(
    _In_ ULONG Group,
    _In_ ULONG SortColumn,
    _In_ PH_SORT_ORDER SortOrder
    )
{
    PPH_LIST roots = GroupRoots[Group];
    PPH_GROUP_SORT_ITEM items;
    PH_TREENEW_COLUMN column;
    BOOLEAN useMetric = FALSE;

    if (roots->Count < 2)
        return;

    if (SortOrder != NoSortOrder && SortColumn != PHPRTLC_NAME && TreeNew_GetColumn(GroupTreeHandle, SortColumn, &column))
    {
        useMetric = SortColumn == PHPRTLC_CPU || SortColumn == PHPRTLC_PRIVATEBYTES ||
            SortColumn == PHPRTLC_IOTOTALRATE || SortColumn >= PHPRTLC_MAXIMUM;
    }

    items = PhAllocate(roots->Count * sizeof(PH_GROUP_SORT_ITEM));

    for (ULONG i = 0; i < roots->Count; i++)
    {
        PPH_PROCESS_NODE node = roots->Items[i];
        ULONG kind;

        items[i].Node = node;
        items[i].Value = 0;

        if (PhGroupIsAppNode(node))
        {
            items[i].Name = PhpGroupGetProcessName(((PPH_APP_ROW)node)->Leader);

            if (useMetric)
                PhGroupGetAppMetric(node, &column, &kind, &items[i].Value);
        }
        else
        {
            items[i].Name = PhpGroupGetProcessName(node);

            if (useMetric)
                PhGetProcessNodeMetric(node, &column, &kind, &items[i].Value);
        }
    }

    GroupSortByName = !useMetric;
    GroupSortOrder = SortOrder == NoSortOrder ? AscendingSortOrder : SortOrder;
    qsort(items, roots->Count, sizeof(PH_GROUP_SORT_ITEM), PhpGroupSortCompare);

    for (ULONG i = 0; i < roots->Count; i++)
        roots->Items[i] = items[i].Node;

    PhFree(items);
}

// Text of a metric cell of an app row (sum of the members), formatted like the process rows.
static BOOLEAN PhpGroupGetAppCellText(
    _In_ PPH_APP_ROW Row,
    _In_ ULONG ColumnId,
    _Out_ PPH_STRINGREF Text
    )
{
    PH_TREENEW_COLUMN column;
    ULONG kind;
    DOUBLE value;
    PH_FORMAT format[2];
    ULONG count;
    ULONG slot;
    SIZE_T returnLength;

    if (!TreeNew_GetColumn(GroupTreeHandle, ColumnId, &column))
        return FALSE;

    if (!PhGroupGetAppMetric(&Row->Node, &column, &kind, &value))
        return FALSE;

    switch (kind)
    {
    case PH_HEAT_KIND_CPU:
        if (value < 0.005)
            return FALSE;
        PhInitFormatF(&format[0], (FLOAT)value, PhMaxPrecisionUnit);
        count = 1;
        break;
    case PH_HEAT_KIND_GPU:
        if (value < 0.01)
            return FALSE;
        PhInitFormatF(&format[0], (FLOAT)value, 2);
        count = 1;
        break;
    case PH_HEAT_KIND_MEMORY:
        if (PhMetricUnits)
        {
            PhMetricInitBytes(format, (ULONG64)value);
        }
        else
        {
            PhInitFormatSize(&format[0], (ULONG64)value);
            PhInitFormatS(&format[1], L"");
        }
        count = 2;
        break;
    case PH_HEAT_KIND_DISK:
    case PH_HEAT_KIND_NETWORK:
        if (value < 1)
            return FALSE;
        if (PhMetricUnits)
        {
            if (kind == PH_HEAT_KIND_DISK)
                PhMetricInitByteRate(format, (ULONG64)value);
            else
                PhMetricInitBitRate(format, (ULONG64)value);
        }
        else
        {
            PhInitFormatSize(&format[0], (ULONG64)value);
            PhInitFormatS(&format[1], L"/s");
        }
        count = 2;
        break;
    default:
        return FALSE;
    }

    for (slot = 0; slot < RTL_NUMBER_OF(Row->Text) - 1; slot++)
    {
        if (Row->TextColumn[slot] == ColumnId || Row->TextColumn[slot] == 0)
            break;
    }

    Row->TextColumn[slot] = ColumnId;
    Row->Text[slot][0] = UNICODE_NULL;

    if (!PhFormatToBuffer(format, count, Row->Text[slot], sizeof(Row->Text[slot]), &returnLength))
        return FALSE;

    Text->Buffer = Row->Text[slot];
    Text->Length = returnLength - sizeof(UNICODE_NULL);
    return TRUE;
}

static VOID PhpGroupEndAppTask(
    _In_ PPH_APP_ROW Row
    )
{
    PPH_PROCESS_ITEM *items;
    ULONG count = 0;

    items = PhAllocate(Row->Members->Count * sizeof(PVOID));

    for (ULONG i = 0; i < Row->Members->Count; i++)
        items[count++] = ((PPH_PROCESS_NODE)Row->Members->Items[i])->ProcessItem;

    if (count)
    {
        PhReferenceObjects(items, count);
        PhUiTerminateProcesses(PhMainWndHandle, items, count);
        PhDereferenceObjects(items, count);
    }

    PhFree(items);
}

#define PH_APP_MENU_END 1
#define PH_APP_MENU_TOGGLE 2

static VOID PhpGroupShowAppMenu(
    _In_ PPH_APP_ROW Row,
    _In_ PPH_TREENEW_CONTEXT_MENU ContextMenu
    )
{
    PPH_EMENU menu = PhCreateEMenu();
    PPH_EMENU_ITEM selected;

    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, PH_APP_MENU_END, L"End &task", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, PH_APP_MENU_TOGGLE, Row->Node.Node.Expanded ? L"&Collapse" : L"&Expand", NULL, NULL), ULONG_MAX);

    selected = PhShowEMenu(menu, GroupTreeHandle, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, ContextMenu->Location.x, ContextMenu->Location.y);

    if (selected && selected->Id == PH_APP_MENU_END)
    {
        PhpGroupEndAppTask(Row);
    }
    else if (selected && selected->Id == PH_APP_MENU_TOGGLE)
    {
        Row->Node.Node.Expanded = !Row->Node.Node.Expanded;
        TreeNew_NodesStructured(GroupTreeHandle);
    }

    PhDestroyEMenu(menu);
}

VOID PhGroupSetByApp(
    _In_ BOOLEAN Enabled,
    _In_ PPH_LIST NodeList
    )
{
    PhSetIntegerSetting(SETTING_GROUP_BY_APP, Enabled);
    GroupByAppEnabled = Enabled;

    if (Enabled && !GroupEnabled)
    {
        PhSetIntegerSetting(SETTING_GROUP_PROCESSES, TRUE);
        GroupEnabled = TRUE;
    }

    if (!Enabled)
        PhpGroupDetachAll(NodeList);

    PhGroupRebuild(NodeList);
    TreeNew_NodesStructured(GroupTreeHandle);
}

VOID PhGroupInitialization(
    _In_ HWND TreeNewHandle
    )
{
    ULONG i;

    GroupTreeHandle = TreeNewHandle;
    GroupEnabled = !!PhGetIntegerSetting(SETTING_GROUP_PROCESSES);
    GroupByAppEnabled = PhGetIntegerSetting(SETTING_GROUP_BY_APP) == 1; // TaskManagerPlus: 0xff (unresolved) counts as off

    if (!AppRows)
    {
        AppRows = PhCreateList(64);
        PhpGroupLoadExpanded();
        WindowsDirectory.Buffer = USER_SHARED_DATA->NtSystemRoot;
        WindowsDirectory.Length = wcslen(USER_SHARED_DATA->NtSystemRoot) * sizeof(WCHAR);
    }

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
    if (!Node)
        return FALSE;

    for (ULONG i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        if (Node == &GroupHeaderNodes[i])
            return TRUE;
    }

    return FALSE;
}

VOID PhGroupSetEnabled(
    _In_ BOOLEAN Enabled,
    _In_ PPH_LIST NodeList
    )
{
    PhSetIntegerSetting(SETTING_GROUP_PROCESSES, Enabled);
    GroupEnabled = Enabled;

    if (!Enabled)
        PhpGroupDetachAll(NodeList); // TaskManagerPlus: app rows only exist inside the sections

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

    if (GroupByAppEnabled) // TaskManagerPlus
    {
        changed = PhpGroupRebuildApps(NodeList);
        goto UpdateHeaders;
    }

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

UpdateHeaders:
    for (i = 0; i < PH_PROCESS_GROUP_COUNT; i++)
    {
        PH_FORMAT format[3];
        SIZE_T returnLength;

        PhInitFormatS(&format[0], i == PH_PROCESS_GROUP_APPS ? L"Apps (" : (i == PH_PROCESS_GROUP_WINDOWS ? L"Windows processes (" : L"Background processes ("));
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

        FreeDeadRows = TRUE; // TaskManagerPlus: only here, after the previous tick's restructure
        if (PhGroupRebuild(NodeList))
            TreeNew_NodesStructured(GroupTreeHandle);
        FreeDeadRows = FALSE;

        if (GroupByAppEnabled)
            PhpGroupSaveExpanded();
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

        if (GroupByAppEnabled) // TaskManagerPlus: app rows + single processes, sorted here by their combined numbers
        {
            PhpGroupSortRoots(group, *SortColumn, SortOrder);
            GetChildren->Children = (PPH_TREENEW_NODE *)GroupRoots[group]->Items;
            GetChildren->NumberOfChildren = GroupRoots[group]->Count;
            return TRUE;
        }

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

    if (PhGroupIsAppNode(node)) // TaskManagerPlus: the members of an app row, flat, in the active sort
    {
        PPH_APP_ROW row = (PPH_APP_ROW)node;

        GetChildren->Children = (PPH_TREENEW_NODE *)row->Members->Items;
        GetChildren->NumberOfChildren = row->Members->Count;
        *SortList = row->Members;

        if (SortOrder == NoSortOrder)
        {
            *SortColumn = DefaultChildSortColumn;
            *SortColumnOrder = AscendingSortOrder;
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

    if (GroupByAppEnabled) // TaskManagerPlus
    {
        for (i = 0; i < AppRows->Count; i++)
        {
            PPH_APP_ROW row = AppRows->Items[i];

            if (row->Node.Node.Expanded != Expand)
            {
                row->Node.Node.Expanded = Expand;
                changed = TRUE;
            }
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

            if (PhMetricUnits)
            {
                PhMetricInitByteRate(format, rate); // TaskManagerPlus: fixed MB/s
            }
            else
            {
                PhInitFormatSizeWithPrecision(&format[0], rate, 1); // short, e.g. 321.3 kB/s
                PhInitFormatS(&format[1], L"/s");
            }
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

    // TaskManagerPlus: app rows. Every message that carries a { Flags, Node } block and could reach the generic
    // code (which dereferences ProcessItem) is answered here.
    if (GroupByAppEnabled)
    {
        switch (Message)
        {
        case TreeNewIsLeaf:
        case TreeNewGetCellText:
        case TreeNewGetNodeColor:
        case TreeNewGetNodeIcon:
        case TreeNewGetNodeFont:
        case TreeNewGetCellTooltip:
        case TreeNewCustomDraw:
            if (PhGroupIsAppNode(nodeBlock->Node))
            {
                PPH_APP_ROW row = (PPH_APP_ROW)nodeBlock->Node;

                *Result = TRUE;

                switch (Message)
                {
                case TreeNewIsLeaf:
                    ((PPH_TREENEW_IS_LEAF)Parameter1)->IsLeaf = row->Members->Count == 0;
                    break;
                case TreeNewGetCellText:
                    {
                        PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;

                        if (getCellText->Id == PHPRTLC_NAME)
                            PhInitializeStringRefLongHint(&getCellText->Text, row->Name);
                        else
                            PhpGroupGetAppCellText(row, getCellText->Id, &getCellText->Text);
                    }
                    break;
                case TreeNewGetNodeColor: // default colours, so the heat tint applies like on process rows
                    *Result = FALSE;
                    break;
                case TreeNewGetNodeIcon:
                    ((PPH_TREENEW_GET_NODE_ICON)Parameter1)->Icon = row->Leader ?
                        (HICON)(ULONG_PTR)row->Leader->ProcessItem->SmallIconIndex : NULL;
                    break;
                default:
                    *Result = FALSE; // defaults
                    break;
                }

                return TRUE;
            }
            break;
        case TreeNewLeftDoubleClick:
        case TreeNewRightClick:
        case TreeNewMiddleClick:
        case TreeNewRightDoubleClick:
            {
                PPH_TREENEW_MOUSE_EVENT mouseEvent = Parameter1;

                if (PhGroupIsAppNode(mouseEvent->Node))
                {
                    if (Message == TreeNewLeftDoubleClick)
                    {
                        mouseEvent->Node->Expanded = !mouseEvent->Node->Expanded;
                        TreeNew_NodesStructured(GroupTreeHandle);
                    }

                    return TRUE;
                }
            }
            break;
        case TreeNewContextMenu:
            {
                PPH_TREENEW_CONTEXT_MENU contextMenu = Parameter1;

                if (PhGroupIsAppNode(contextMenu->Node))
                {
                    PhpGroupShowAppMenu((PPH_APP_ROW)contextMenu->Node, contextMenu);
                    return TRUE;
                }
            }
            break;
        case TreeNewNodeExpanding: // Parameter1 is the node itself
            if (PhGroupIsAppNode(Parameter1))
                return TRUE;
            break;
        }
    }

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
