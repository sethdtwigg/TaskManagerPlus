/*
 * TaskManagerPlus Process History plugin: the "History" main-window tab.
 *
 * The tab page window is a container of our own class that holds a trackbar
 * (one position per snapshot, right end = latest), a "Live" checkbox, a
 * status label and a TreeNew listing the processes of the selected snapshot.
 */

#include "processhistory.h"

#define HIST_TAB_CLASS L"TaskManagerPlusProcessHistoryTab"

#define HIST_CONTROL_ID_TRACKBAR 1001
#define HIST_CONTROL_ID_LIVE 1002
#define HIST_CONTROL_ID_LABEL 1003

typedef enum _HIST_COLUMN
{
    HIST_COLUMN_NAME,
    HIST_COLUMN_PID,
    HIST_COLUMN_PARENTPID,
    HIST_COLUMN_STATUS,
    HIST_COLUMN_CPU,
    HIST_COLUMN_PRIVATEBYTES,
    HIST_COLUMN_WORKINGSET,
    HIST_COLUMN_THREADS,
    HIST_COLUMN_HANDLES,
    HIST_COLUMN_SESSION,
    HIST_COLUMN_STARTTIME,
    HIST_COLUMN_PATH,
    HIST_COLUMN_MAXIMUM
} HIST_COLUMN;

typedef enum _HIST_STATUS
{
    HIST_STATUS_RUNNING,
    HIST_STATUS_NEW,
    HIST_STATUS_EXITED,
    HIST_STATUS_EXITED_LATER,
    HIST_STATUS_MAXIMUM
} HIST_STATUS;

static CONST PH_STRINGREF HistStatusStrings[HIST_STATUS_MAXIMUM] =
{
    PH_STRINGREF_INIT(L"Running"),
    PH_STRINGREF_INIT(L"New"),
    PH_STRINGREF_INIT(L"Exited"),
    PH_STRINGREF_INIT(L"Exited later"),
};

typedef struct _HIST_NODE
{
    PH_TREENEW_NODE Node;
    PH_STRINGREF TextCache[HIST_COLUMN_MAXIMUM];

    HIST_ENTRY Entry;
    HIST_STATUS Status;

    PPH_STRING Name; // referenced from the store's string table
    PPH_STRING Path; // referenced from the store's string table
    PPH_STRING ProcessIdString;
    PPH_STRING ParentProcessIdString;
    PPH_STRING CpuString;
    PPH_STRING PrivateBytesString;
    PPH_STRING WorkingSetString;
    PPH_STRING ThreadsString;
    PPH_STRING HandlesString;
    PPH_STRING SessionString;
    PPH_STRING StartTimeString;
} HIST_NODE, *PHIST_NODE;

typedef enum _HIST_COMMAND_ID
{
    HIST_COMMAND_ID_GOTOPROCESS = 1,
    HIST_COMMAND_ID_COPY,
    HIST_COMMAND_ID_CLEAR,
} HIST_COMMAND_ID;

static CONST PH_STRINGREF HistTabPageText = PH_STRINGREF_INIT(L"History");
static CONST PH_STRINGREF HistTabEmptyText = PH_STRINGREF_INIT(L"No snapshots have been recorded yet.");

static PPH_MAIN_TAB_PAGE HistTabPage = NULL;
static HWND HistTabWindowHandle = NULL;
static HWND HistTrackbarHandle = NULL;
static HWND HistLiveHandle = NULL;
static HWND HistLabelHandle = NULL;
static HWND HistTreeNewHandle = NULL;
static HFONT HistFontHandle = NULL;
static BOOLEAN HistTreeNewCreated = FALSE;
static PPH_LIST HistNodeList = NULL;
static ULONG HistSortColumn = HIST_COLUMN_NAME;
static PH_SORT_ORDER HistSortOrder = AscendingSortOrder;
static COLORREF HistColorNew = 0;
static COLORREF HistColorRemoved = 0;
static BOOLEAN HistLiveMode = TRUE;
static LONGLONG HistDisplayedTime = 0; // Time of the snapshot the tree currently shows (0 = none)
static ULONG HistCountNew = 0;
static ULONG HistCountExited = 0;

static VOID HistLayout(
    VOID
    );

static VOID HistSelectSnapshotIndex(
    _In_ ULONG Index
    );

static VOID HistHandleCommand(
    _In_ ULONG Id
    );

// Nodes

static VOID HistFreeNode(
    _In_ PHIST_NODE Node
    )
{
    PhClearReference(&Node->Name);
    PhClearReference(&Node->Path);
    PhClearReference(&Node->ProcessIdString);
    PhClearReference(&Node->ParentProcessIdString);
    PhClearReference(&Node->CpuString);
    PhClearReference(&Node->PrivateBytesString);
    PhClearReference(&Node->WorkingSetString);
    PhClearReference(&Node->ThreadsString);
    PhClearReference(&Node->HandlesString);
    PhClearReference(&Node->SessionString);
    PhClearReference(&Node->StartTimeString);
    PhFree(Node);
}

static VOID HistClearNodes(
    VOID
    )
{
    for (ULONG i = 0; i < HistNodeList->Count; i++)
        HistFreeNode(HistNodeList->Items[i]);

    PhClearList(HistNodeList);
}

static PPH_STRING HistFormatUInt32(
    _In_ ULONG Value
    )
{
    PH_FORMAT format[1];

    PhInitFormatU(&format[0], Value);

    return PhFormat(format, RTL_NUMBER_OF(format), 0);
}

static PHIST_NODE HistCreateNode(
    _In_ PHIST_ENTRY Entry
    )
{
    PHIST_NODE node;
    PPH_STRING string;
    PH_FORMAT format[2];

    node = PhAllocateZero(sizeof(HIST_NODE));
    PhInitializeTreeNewNode(&node->Node);
    node->Node.TextCache = node->TextCache;
    node->Node.TextCacheSize = HIST_COLUMN_MAXIMUM;
    node->Entry = *Entry;
    node->Status = HIST_STATUS_RUNNING;

    if (string = HistStoreGetString(Entry->NameIndex))
        PhSetReference(&node->Name, string);
    if (string = HistStoreGetString(Entry->PathIndex))
        PhSetReference(&node->Path, string);

    node->ProcessIdString = HistFormatUInt32(Entry->ProcessId);
    node->ParentProcessIdString = HistFormatUInt32(Entry->ParentProcessId);
    node->ThreadsString = HistFormatUInt32(Entry->NumberOfThreads);
    node->HandlesString = HistFormatUInt32(Entry->NumberOfHandles);
    node->SessionString = HistFormatUInt32(Entry->SessionId);

    if (Entry->CpuUsage > 0.0f)
    {
        PhInitFormatF(&format[0], Entry->CpuUsage * 100.0f, 2);
        node->CpuString = PhFormat(format, 1, 0);
    }

    node->PrivateBytesString = PhFormatSize(Entry->PrivateBytes, ULONG_MAX);
    node->WorkingSetString = PhFormatSize(Entry->WorkingSetSize, ULONG_MAX);

    if (Entry->CreateTime != 0)
    {
        LARGE_INTEGER createTime;
        SYSTEMTIME systemTime;

        createTime.QuadPart = Entry->CreateTime;
        PhLargeIntegerToLocalSystemTime(&systemTime, &createTime);
        node->StartTimeString = PhFormatDateTime(&systemTime);
    }

    return node;
}

/**
 * Recomputes the New/Exited status of every node against the neighbouring
 * snapshots. Returns TRUE if any status changed.
 */
static BOOLEAN HistUpdateStatuses(
    _In_ ULONG Index
    )
{
    PHIST_SNAPSHOT previous;
    PHIST_SNAPSHOT next;
    PHIST_SNAPSHOT latest;
    ULONG count;
    BOOLEAN changed = FALSE;

    count = HistStoreGetSnapshotCount();
    previous = Index > 0 ? HistStoreGetSnapshot(Index - 1) : NULL;
    next = HistStoreGetSnapshot(Index + 1);
    latest = count ? HistStoreGetSnapshot(count - 1) : NULL;

    HistCountNew = 0;
    HistCountExited = 0;

    for (ULONG i = 0; i < HistNodeList->Count; i++)
    {
        PHIST_NODE node = HistNodeList->Items[i];
        HIST_STATUS status = HIST_STATUS_RUNNING;

        if (previous && !HistSnapshotFindEntry(previous, node->Entry.ProcessId, node->Entry.CreateTime))
            status = HIST_STATUS_NEW;
        else if (next && !HistSnapshotFindEntry(next, node->Entry.ProcessId, node->Entry.CreateTime))
            status = HIST_STATUS_EXITED;
        else if (next && latest && latest != next && !HistSnapshotFindEntry(latest, node->Entry.ProcessId, node->Entry.CreateTime))
            status = HIST_STATUS_EXITED_LATER;

        if (status == HIST_STATUS_NEW)
            HistCountNew++;
        else if (status == HIST_STATUS_EXITED)
            HistCountExited++;

        if (node->Status != status)
        {
            node->Status = status;
            PhInitializeEmptyStringRef(&node->TextCache[HIST_COLUMN_STATUS]);
            PhInvalidateTreeNewNode(&node->Node, TN_CACHE_COLOR);
            changed = TRUE;
        }
    }

    return changed;
}

static VOID HistUpdateLabel(
    _In_opt_ PHIST_SNAPSHOT Snapshot,
    _In_ ULONG Index
    )
{
    PPH_STRING text;

    if (Snapshot)
    {
        SYSTEMTIME systemTime;
        PPH_STRING timeString;

        PhLargeIntegerToLocalSystemTime(&systemTime, &Snapshot->Time);
        timeString = PhFormatDateTime(&systemTime);

        text = PhFormatString(
            L"Snapshot %lu of %lu: %s (%lu processes, %lu new, %lu exited)",
            Index + 1,
            HistStoreGetSnapshotCount(),
            PhGetString(timeString),
            HistNodeList->Count,
            HistCountNew,
            HistCountExited
            );

        PhDereferenceObject(timeString);
    }
    else
    {
        text = PhCreateString(L"No snapshots have been recorded yet.");
    }

    PhSetWindowText(HistLabelHandle, PhGetString(text));
    PhDereferenceObject(text);
}

static VOID HistUpdateTrackbar(
    _In_ ULONG Index
    )
{
    ULONG count = HistStoreGetSnapshotCount();

    SendMessage(HistTrackbarHandle, TBM_SETRANGEMIN, FALSE, 0);
    SendMessage(HistTrackbarHandle, TBM_SETRANGEMAX, TRUE, count ? count - 1 : 0);
    SendMessage(HistTrackbarHandle, TBM_SETPOS, TRUE, count ? Index : 0);
    EnableWindow(HistTrackbarHandle, count > 1);
}

/**
 * Shows the snapshot at the given index. Rebuilds the node list when the
 * snapshot differs from the one currently displayed; otherwise only refreshes
 * the statuses (which depend on neighbouring snapshots).
 */
static VOID HistSelectSnapshotIndex(
    _In_ ULONG Index
    )
{
    PHIST_SNAPSHOT snapshot;
    ULONG count;

    count = HistStoreGetSnapshotCount();

    if (count == 0)
    {
        HistClearNodes();
        HistDisplayedTime = 0;
        HistCountNew = 0;
        HistCountExited = 0;
        HistUpdateTrackbar(0);
        HistUpdateLabel(NULL, 0);
        TreeNew_NodesStructured(HistTreeNewHandle);
        return;
    }

    if (Index >= count)
        Index = count - 1;

    snapshot = HistStoreGetSnapshot(Index);

    if (snapshot->Time.QuadPart != HistDisplayedTime)
    {
        TreeNew_SetRedraw(HistTreeNewHandle, FALSE);
        HistClearNodes();

        for (ULONG i = 0; i < snapshot->Count; i++)
            PhAddItemList(HistNodeList, HistCreateNode(&snapshot->Entries[i]));

        HistDisplayedTime = snapshot->Time.QuadPart;
        HistUpdateStatuses(Index);
        TreeNew_NodesStructured(HistTreeNewHandle);
        TreeNew_SetRedraw(HistTreeNewHandle, TRUE);
    }
    else
    {
        if (HistUpdateStatuses(Index))
        {
            if (HistSortColumn == HIST_COLUMN_STATUS && HistSortOrder != NoSortOrder)
                TreeNew_NodesStructured(HistTreeNewHandle);
            else
                InvalidateRect(HistTreeNewHandle, NULL, FALSE);
        }
    }

    HistUpdateTrackbar(Index);
    HistUpdateLabel(snapshot, Index);
}

/**
 * Called by the store whenever snapshots are added, pruned or cleared.
 */
VOID HistTabSnapshotsChanged(
    VOID
    )
{
    ULONG count;
    ULONG index;

    if (!HistTreeNewCreated)
        return;

    count = HistStoreGetSnapshotCount();

    if (HistLiveMode || count == 0)
    {
        index = count ? count - 1 : 0;
    }
    else
    {
        // Keep showing the same moment in time if it still exists; if it was
        // pruned, fall back to the oldest snapshot.
        index = 0;

        for (ULONG i = 0; i < count; i++)
        {
            PHIST_SNAPSHOT snapshot = HistStoreGetSnapshot(i);

            if (snapshot->Time.QuadPart >= HistDisplayedTime)
            {
                index = i;
                break;
            }
        }
    }

    HistSelectSnapshotIndex(index);
}

// Selection helpers

static PHIST_NODE HistGetSelectedNode(
    VOID
    )
{
    for (ULONG i = 0; i < HistNodeList->Count; i++)
    {
        PHIST_NODE node = HistNodeList->Items[i];

        if (node->Node.Selected)
            return node;
    }

    return NULL;
}

static ULONG HistGetSelectedCount(
    VOID
    )
{
    ULONG count = 0;

    for (ULONG i = 0; i < HistNodeList->Count; i++)
    {
        PHIST_NODE node = HistNodeList->Items[i];

        if (node->Node.Selected)
            count++;
    }

    return count;
}

// Sorting

#define SORT_FUNCTION(Column) HistTreeNewCompare##Column
#define BEGIN_SORT_FUNCTION(Column) static int __cdecl HistTreeNewCompare##Column( \
    _In_ void const* _elem1, \
    _In_ void const* _elem2 \
    ) \
{ \
    PHIST_NODE node1 = *(PHIST_NODE *)_elem1; \
    PHIST_NODE node2 = *(PHIST_NODE *)_elem2; \
    int sortResult = 0;

#define END_SORT_FUNCTION \
    if (sortResult == 0) \
        sortResult = uintcmp(node1->Entry.ProcessId, node2->Entry.ProcessId); \
    \
    return PhModifySort(sortResult, HistSortOrder); \
}

BEGIN_SORT_FUNCTION(Name)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->Name, node2->Name, HistSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(ProcessId)
{
    sortResult = uintcmp(node1->Entry.ProcessId, node2->Entry.ProcessId);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(ParentProcessId)
{
    sortResult = uintcmp(node1->Entry.ParentProcessId, node2->Entry.ParentProcessId);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Status)
{
    sortResult = uintcmp(node1->Status, node2->Status);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Cpu)
{
    sortResult = singlecmp(node1->Entry.CpuUsage, node2->Entry.CpuUsage);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(PrivateBytes)
{
    sortResult = uint64cmp(node1->Entry.PrivateBytes, node2->Entry.PrivateBytes);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(WorkingSet)
{
    sortResult = uint64cmp(node1->Entry.WorkingSetSize, node2->Entry.WorkingSetSize);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Threads)
{
    sortResult = uintcmp(node1->Entry.NumberOfThreads, node2->Entry.NumberOfThreads);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Handles)
{
    sortResult = uintcmp(node1->Entry.NumberOfHandles, node2->Entry.NumberOfHandles);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Session)
{
    sortResult = uintcmp(node1->Entry.SessionId, node2->Entry.SessionId);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(StartTime)
{
    sortResult = int64cmp(node1->Entry.CreateTime, node2->Entry.CreateTime);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Path)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->Path, node2->Path, HistSortOrder, TRUE);
}
END_SORT_FUNCTION

static int __cdecl HistTreeNewNoOrderSortFunction(
    _In_ void const* _elem1,
    _In_ void const* _elem2
    )
{
    PHIST_NODE node1 = *(PHIST_NODE*)_elem1;
    PHIST_NODE node2 = *(PHIST_NODE*)_elem2;

    return uintcmp(node1->Entry.ProcessId, node2->Entry.ProcessId);
}

// Settings

static VOID HistLoadTreeListSettings(
    VOID
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;

    settings = PhGetStringSetting(SETTING_NAME_TREE_LIST_COLUMNS);
    PhCmLoadSettings(HistTreeNewHandle, &settings->sr);
    PhDereferenceObject(settings);

    sortSettings = PhGetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT);
    TreeNew_SetSort(HistTreeNewHandle, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);
}

static VOID HistSaveTreeListSettings(
    VOID
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;
    ULONG sortColumn;
    PH_SORT_ORDER sortOrder;

    if (!HistTreeNewCreated)
        return;

    settings = PhCmSaveSettings(HistTreeNewHandle);
    PhSetStringSetting2(SETTING_NAME_TREE_LIST_COLUMNS, &settings->sr);
    PhDereferenceObject(settings);

    TreeNew_GetSort(HistTreeNewHandle, &sortColumn, &sortOrder);
    sortSettings.X = sortColumn;
    sortSettings.Y = sortOrder;
    PhSetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT, sortSettings);
}

// Commands

static VOID HistCopyList(
    VOID
    )
{
    PPH_STRING text;

    text = PhGetTreeNewText(HistTreeNewHandle, 0);
    PhSetClipboardString(HistTreeNewHandle, &text->sr);
    PhDereferenceObject(text);
}

static VOID HistWriteList(
    _In_ PPH_FILE_STREAM FileStream,
    _In_ ULONG Mode
    )
{
    PPH_LIST lines = PhGetGenericTreeNewLines(HistTreeNewHandle, Mode);

    for (ULONG i = 0; i < lines->Count; i++)
    {
        PPH_STRING line = lines->Items[i];

        PhWriteStringAsUtf8FileStream(FileStream, &line->sr);
        PhDereferenceObject(line);
        PhWriteStringAsUtf8FileStream2(FileStream, L"\r\n");
    }

    PhDereferenceObject(lines);
}

static VOID HistHandleCommand(
    _In_ ULONG Id
    )
{
    switch (Id)
    {
    case HIST_COMMAND_ID_GOTOPROCESS:
        {
            PHIST_NODE node;
            PPH_PROCESS_NODE processNode;

            if (!(node = HistGetSelectedNode()))
                break;

            processNode = PhFindProcessNode(UlongToHandle(node->Entry.ProcessId));

            if (processNode && processNode->ProcessItem &&
                processNode->ProcessItem->CreateTime.QuadPart == node->Entry.CreateTime)
            {
                SystemInformer_SelectTabPage(0);
                SystemInformer_SelectProcessNode(processNode);
            }
            else
            {
                PhShowInformation2(
                    HistTreeNewHandle,
                    L"Process History",
                    L"%s (PID %lu) is no longer running.",
                    PhGetStringOrDefault(node->Name, L"The process"),
                    node->Entry.ProcessId
                    );
            }
        }
        break;
    case HIST_COMMAND_ID_COPY:
        {
            HistCopyList();
        }
        break;
    case HIST_COMMAND_ID_CLEAR:
        {
            if (PhShowMessage2(
                HistTreeNewHandle,
                TD_YES_BUTTON | TD_NO_BUTTON,
                TD_WARNING_ICON,
                L"Clear process history?",
                L"%s",
                L"This removes every recorded snapshot from memory and deletes the history file on disk."
                ) == IDYES)
            {
                HistStoreClear();
            }
        }
        break;
    }
}

static VOID HistShowContextMenu(
    _In_ PPH_TREENEW_CONTEXT_MENU ContextMenuEvent
    )
{
    PPH_EMENU menu;
    PPH_EMENU_ITEM item;
    ULONG selectedCount;

    selectedCount = HistGetSelectedCount();

    menu = PhCreateEMenu();
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, HIST_COMMAND_ID_GOTOPROCESS, L"&Go to process", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, HIST_COMMAND_ID_CLEAR, L"Clear &history...", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, HIST_COMMAND_ID_COPY, L"&Copy\bCtrl+C", NULL, NULL), ULONG_MAX);
    PhInsertCopyCellEMenuItem(menu, HIST_COMMAND_ID_COPY, HistTreeNewHandle, ContextMenuEvent->Column);
    PhSetFlagsEMenuItem(menu, HIST_COMMAND_ID_GOTOPROCESS, PH_EMENU_DEFAULT, PH_EMENU_DEFAULT);

    if (selectedCount != 1)
        PhEnableEMenuItem(menu, HIST_COMMAND_ID_GOTOPROCESS, FALSE);
    if (selectedCount == 0)
        PhEnableEMenuItem(menu, HIST_COMMAND_ID_COPY, FALSE);
    if (HistStoreGetSnapshotCount() == 0)
        PhEnableEMenuItem(menu, HIST_COMMAND_ID_CLEAR, FALSE);

    item = PhShowEMenu(
        menu,
        HistTreeNewHandle,
        PH_EMENU_SHOW_LEFTRIGHT,
        PH_ALIGN_LEFT | PH_ALIGN_TOP,
        ContextMenuEvent->Location.x,
        ContextMenuEvent->Location.y
        );

    if (item)
    {
        if (!PhHandleCopyCellEMenuItem(item))
            HistHandleCommand(item->Id);
    }

    PhDestroyEMenu(menu);
}

// TreeNew callback

static BOOLEAN NTAPI HistTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_opt_ PVOID Context
    )
{
    PHIST_NODE node;

    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;

            if (getChildren->Node)
                break;

            if (HistSortOrder == NoSortOrder)
            {
                qsort(HistNodeList->Items, HistNodeList->Count, sizeof(PVOID), HistTreeNewNoOrderSortFunction);
            }
            else
            {
                static CONST _CoreCrtNonSecureSearchSortCompareFunction sortFunctions[] =
                {
                    SORT_FUNCTION(Name),
                    SORT_FUNCTION(ProcessId),
                    SORT_FUNCTION(ParentProcessId),
                    SORT_FUNCTION(Status),
                    SORT_FUNCTION(Cpu),
                    SORT_FUNCTION(PrivateBytes),
                    SORT_FUNCTION(WorkingSet),
                    SORT_FUNCTION(Threads),
                    SORT_FUNCTION(Handles),
                    SORT_FUNCTION(Session),
                    SORT_FUNCTION(StartTime),
                    SORT_FUNCTION(Path),
                };

                static_assert(RTL_NUMBER_OF(sortFunctions) == HIST_COLUMN_MAXIMUM, "SortFunctions must equal maximum.");

                if (HistSortColumn < HIST_COLUMN_MAXIMUM)
                    qsort(HistNodeList->Items, HistNodeList->Count, sizeof(PVOID), sortFunctions[HistSortColumn]);
            }

            getChildren->Children = (PPH_TREENEW_NODE*)HistNodeList->Items;
            getChildren->NumberOfChildren = HistNodeList->Count;
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
            node = (PHIST_NODE)getCellText->Node;

            switch (getCellText->Id)
            {
            case HIST_COLUMN_NAME:
                getCellText->Text = PhGetStringRef(node->Name);
                break;
            case HIST_COLUMN_PID:
                getCellText->Text = PhGetStringRef(node->ProcessIdString);
                break;
            case HIST_COLUMN_PARENTPID:
                getCellText->Text = PhGetStringRef(node->ParentProcessIdString);
                break;
            case HIST_COLUMN_STATUS:
                getCellText->Text = HistStatusStrings[node->Status];
                break;
            case HIST_COLUMN_CPU:
                getCellText->Text = PhGetStringRef(node->CpuString);
                break;
            case HIST_COLUMN_PRIVATEBYTES:
                getCellText->Text = PhGetStringRef(node->PrivateBytesString);
                break;
            case HIST_COLUMN_WORKINGSET:
                getCellText->Text = PhGetStringRef(node->WorkingSetString);
                break;
            case HIST_COLUMN_THREADS:
                getCellText->Text = PhGetStringRef(node->ThreadsString);
                break;
            case HIST_COLUMN_HANDLES:
                getCellText->Text = PhGetStringRef(node->HandlesString);
                break;
            case HIST_COLUMN_SESSION:
                getCellText->Text = PhGetStringRef(node->SessionString);
                break;
            case HIST_COLUMN_STARTTIME:
                getCellText->Text = PhGetStringRef(node->StartTimeString);
                break;
            case HIST_COLUMN_PATH:
                getCellText->Text = PhGetStringRef(node->Path);
                break;
            default:
                return FALSE;
            }

            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewGetNodeColor:
        {
            PPH_TREENEW_GET_NODE_COLOR getNodeColor = Parameter1;
            node = (PHIST_NODE)getNodeColor->Node;

            switch (node->Status)
            {
            case HIST_STATUS_NEW:
                getNodeColor->BackColor = HistColorNew;
                break;
            case HIST_STATUS_EXITED:
                getNodeColor->BackColor = HistColorRemoved;
                break;
            }

            getNodeColor->Flags = TN_CACHE | TN_AUTO_FORECOLOR;
        }
        return TRUE;
    case TreeNewSortChanged:
        {
            PPH_TREENEW_SORT_CHANGED_EVENT sorting = Parameter1;

            HistSortColumn = sorting->SortColumn;
            HistSortOrder = sorting->SortOrder;

            TreeNew_NodesStructured(WindowHandle);
        }
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            switch (keyEvent->VirtualKey)
            {
            case 'C':
                if (GetKeyState(VK_CONTROL) < 0)
                    HistHandleCommand(HIST_COMMAND_ID_COPY);
                break;
            case VK_RETURN:
                HistHandleCommand(HIST_COMMAND_ID_GOTOPROCESS);
                break;
            }
        }
        return TRUE;
    case TreeNewHeaderRightClick:
        {
            PH_TN_COLUMN_MENU_DATA data;

            data.TreeNewHandle = WindowHandle;
            data.MouseEvent = Parameter1;
            data.DefaultSortColumn = HIST_COLUMN_NAME;
            data.DefaultSortOrder = AscendingSortOrder;
            PhInitializeTreeNewColumnMenuEx(&data, PH_TN_COLUMN_MENU_SHOW_RESET_SORT);

            data.Selection = PhShowEMenu(
                data.Menu,
                WindowHandle,
                PH_EMENU_SHOW_LEFTRIGHT,
                PH_ALIGN_LEFT | PH_ALIGN_TOP,
                data.MouseEvent->ScreenLocation.x,
                data.MouseEvent->ScreenLocation.y
                );
            PhHandleTreeNewColumnMenu(&data);
            PhDeleteTreeNewColumnMenu(&data);
        }
        return TRUE;
    case TreeNewLeftDoubleClick:
        {
            HistHandleCommand(HIST_COMMAND_ID_GOTOPROCESS);
        }
        return TRUE;
    case TreeNewContextMenu:
        {
            HistShowContextMenu(Parameter1);
        }
        return TRUE;
    }

    return FALSE;
}

static VOID HistInitializeTreeList(
    VOID
    )
{
    HistNodeList = PhCreateList(512);

    PhSetControlTheme(HistTreeNewHandle, L"explorer");
    TreeNew_SetRedraw(HistTreeNewHandle, FALSE);
    TreeNew_SetCallback(HistTreeNewHandle, HistTreeNewCallback, NULL);

    PhAddTreeNewColumn(HistTreeNewHandle, HIST_COLUMN_NAME, TRUE, L"Name", 160, PH_ALIGN_LEFT, HIST_COLUMN_NAME, 0);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_PID, TRUE, L"PID", 55, PH_ALIGN_RIGHT, HIST_COLUMN_PID, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_PARENTPID, TRUE, L"Parent PID", 70, PH_ALIGN_RIGHT, HIST_COLUMN_PARENTPID, DT_RIGHT, TRUE);
    PhAddTreeNewColumn(HistTreeNewHandle, HIST_COLUMN_STATUS, TRUE, L"Status", 80, PH_ALIGN_LEFT, HIST_COLUMN_STATUS, 0);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_CPU, TRUE, L"CPU", 50, PH_ALIGN_RIGHT, HIST_COLUMN_CPU, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_PRIVATEBYTES, TRUE, L"Private bytes", 90, PH_ALIGN_RIGHT, HIST_COLUMN_PRIVATEBYTES, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_WORKINGSET, TRUE, L"Working set", 90, PH_ALIGN_RIGHT, HIST_COLUMN_WORKINGSET, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_THREADS, TRUE, L"Threads", 60, PH_ALIGN_RIGHT, HIST_COLUMN_THREADS, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_HANDLES, TRUE, L"Handles", 60, PH_ALIGN_RIGHT, HIST_COLUMN_HANDLES, DT_RIGHT, TRUE);
    PhAddTreeNewColumnEx(HistTreeNewHandle, HIST_COLUMN_SESSION, FALSE, L"Session", 50, PH_ALIGN_RIGHT, HIST_COLUMN_SESSION, DT_RIGHT, TRUE);
    PhAddTreeNewColumn(HistTreeNewHandle, HIST_COLUMN_STARTTIME, TRUE, L"Start time", 140, PH_ALIGN_LEFT, HIST_COLUMN_STARTTIME, 0);
    PhAddTreeNewColumn(HistTreeNewHandle, HIST_COLUMN_PATH, TRUE, L"Path", 300, PH_ALIGN_LEFT, HIST_COLUMN_PATH, DT_PATH_ELLIPSIS);

    if (PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_ROW_SIZE))
    {
        ULONG treelistCustomRowSize = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_ROW_SIZE);

        if (treelistCustomRowSize < 15)
            treelistCustomRowSize = 15;

        TreeNew_SetRowHeight(HistTreeNewHandle, treelistCustomRowSize);
    }

    TreeNew_SetEmptyText(HistTreeNewHandle, &HistTabEmptyText, 0);
    TreeNew_SetSort(HistTreeNewHandle, HIST_COLUMN_NAME, AscendingSortOrder);
    TreeNew_SetTriState(HistTreeNewHandle, TRUE);
    TreeNew_SetRedraw(HistTreeNewHandle, TRUE);

    HistLoadTreeListSettings();
}

// Container window

static VOID HistUpdateFont(
    VOID
    )
{
    HFONT oldFont = HistFontHandle;

    HistFontHandle = PhCreateApplicationFont(PhGetWindowDpi(HistTabWindowHandle));

    if (HistFontHandle)
    {
        SetWindowFont(HistLiveHandle, HistFontHandle, TRUE);
        SetWindowFont(HistLabelHandle, HistFontHandle, TRUE);
    }

    if (oldFont)
        DeleteFont(oldFont);
}

static VOID HistLayout(
    VOID
    )
{
    RECT clientRect;
    LONG dpiValue;
    LONG padding;
    LONG trackbarHeight;
    LONG rowHeight;
    LONG liveWidth;
    LONG width;
    LONG top;
    HDWP deferHandle;

    if (!HistTabWindowHandle || !HistTreeNewHandle)
        return;

    PhGetClientRect(HistTabWindowHandle, &clientRect);
    dpiValue = PhGetWindowDpi(HistTabWindowHandle);
    // TaskManagerPlus: 8px grid padding, comfortable control heights
    padding = PhScaleToDisplay(8, dpiValue);
    trackbarHeight = PhScaleToDisplay(32, dpiValue);
    rowHeight = PhScaleToDisplay(24, dpiValue);
    liveWidth = PhScaleToDisplay(64, dpiValue);
    width = clientRect.right - clientRect.left;

    deferHandle = BeginDeferWindowPos(4);

    top = padding;
    deferHandle = DeferWindowPos(deferHandle, HistTrackbarHandle, NULL,
        padding, top, max(width - 3 * padding - liveWidth, 0), trackbarHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    deferHandle = DeferWindowPos(deferHandle, HistLiveHandle, NULL,
        max(width - padding - liveWidth, 0), top + (trackbarHeight - rowHeight) / 2, liveWidth, rowHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    top += trackbarHeight;
    deferHandle = DeferWindowPos(deferHandle, HistLabelHandle, NULL,
        padding, top, max(width - 2 * padding, 0), rowHeight, SWP_NOZORDER | SWP_NOACTIVATE);

    top += rowHeight + padding;
    deferHandle = DeferWindowPos(deferHandle, HistTreeNewHandle, NULL,
        0, top, width, max(clientRect.bottom - top, 0), SWP_NOZORDER | SWP_NOACTIVATE);

    EndDeferWindowPos(deferHandle);
}

static LRESULT CALLBACK HistTabWndProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_SIZE:
        {
            HistLayout();
        }
        break;
    case WM_ERASEBKGND:
        {
            // TaskManagerPlus: background and child-control colours follow the theme palette
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
            if (HistTreeNewHandle)
                SetFocus(HistTreeNewHandle);
        }
        return 0;
    case WM_HSCROLL:
        {
            if ((HWND)lParam == HistTrackbarHandle && HistTrackbarHandle)
            {
                ULONG position = (ULONG)SendMessage(HistTrackbarHandle, TBM_GETPOS, 0, 0);
                ULONG count = HistStoreGetSnapshotCount();
                BOOLEAN live = count != 0 && position >= count - 1;

                if (live != HistLiveMode)
                {
                    HistLiveMode = live;
                    Button_SetCheck(HistLiveHandle, live ? BST_CHECKED : BST_UNCHECKED);
                }

                HistSelectSnapshotIndex(position);
            }
        }
        return 0;
    case WM_COMMAND:
        {
            if (GET_WM_COMMAND_ID(wParam, lParam) == HIST_CONTROL_ID_LIVE &&
                GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED)
            {
                HistLiveMode = Button_GetCheck(HistLiveHandle) == BST_CHECKED;

                if (HistLiveMode)
                    HistTabSnapshotsChanged();
            }
        }
        return 0;
    case WM_DESTROY:
        {
            if (HistFontHandle)
            {
                DeleteFont(HistFontHandle);
                HistFontHandle = NULL;
            }
        }
        break;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

static BOOLEAN HistRegisterTabClass(
    VOID
    )
{
    static PH_INITONCE initOnce = PH_INITONCE_INIT;
    static ATOM classAtom = 0;

    if (PhBeginInitOnce(&initOnce))
    {
        WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };

        wcex.lpfnWndProc = HistTabWndProc;
        wcex.hInstance = PluginInstance->DllBase;
        wcex.hCursor = PhLoadCursor(NULL, IDC_ARROW);
        wcex.hbrBackground = NULL; // TaskManagerPlus: painted in WM_ERASEBKGND from the theme palette
        wcex.lpszClassName = HIST_TAB_CLASS;

        classAtom = RegisterClassEx(&wcex);

        PhEndInitOnce(&initOnce);
    }

    return classAtom != 0;
}

static BOOLEAN HistCreateTabWindow(
    _In_ HWND ParentWindowHandle
    )
{
    ULONG thinRows;
    ULONG treelistBorder;
    ULONG treelistCustomColors;
    PH_TREENEW_CREATEPARAMS treelistCreateParams = { sizeof(PH_TREENEW_CREATEPARAMS) };
    RECT clientRect = { 0 };
    BOOLEAN themeSupport;

    if (!HistRegisterTabClass())
        return FALSE;

    HistColorNew = PhGetIntegerSetting(L"ColorNew");
    HistColorRemoved = PhGetIntegerSetting(L"ColorRemoved");
    themeSupport = !!PhGetIntegerSetting(SETTING_ENABLE_THEME_SUPPORT);

    PhGetClientRect(ParentWindowHandle, &clientRect);

    HistTabWindowHandle = PhCreateWindow(
        HIST_TAB_CLASS,
        NULL,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        clientRect.left,
        clientRect.top,
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
        ParentWindowHandle,
        NULL,
        PluginInstance->DllBase,
        NULL
        );

    if (!HistTabWindowHandle)
        return FALSE;

    HistTrackbarHandle = CreateWindowEx(
        0,
        TRACKBAR_CLASS,
        NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_TOOLTIPS,
        0, 0, 0, 0,
        HistTabWindowHandle,
        (HMENU)(ULONG_PTR)HIST_CONTROL_ID_TRACKBAR,
        PluginInstance->DllBase,
        NULL
        );

    HistLiveHandle = CreateWindowEx(
        0,
        WC_BUTTON,
        L"Live",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0,
        HistTabWindowHandle,
        (HMENU)(ULONG_PTR)HIST_CONTROL_ID_LIVE,
        PluginInstance->DllBase,
        NULL
        );

    HistLabelHandle = CreateWindowEx(
        0,
        WC_STATIC,
        L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_ENDELLIPSIS,
        0, 0, 0, 0,
        HistTabWindowHandle,
        (HMENU)(ULONG_PTR)HIST_CONTROL_ID_LABEL,
        PluginInstance->DllBase,
        NULL
        );

    if (!HistTrackbarHandle || !HistLiveHandle || !HistLabelHandle)
        return FALSE;

    Button_SetCheck(HistLiveHandle, HistLiveMode ? BST_CHECKED : BST_UNCHECKED);
    HistUpdateFont();

    thinRows = PhGetIntegerSetting(SETTING_THIN_ROWS) ? TN_STYLE_THIN_ROWS : 0;
    treelistBorder = (PhGetIntegerSetting(SETTING_TREE_LIST_BORDER_ENABLE) && !themeSupport) ? WS_BORDER : 0;
    treelistCustomColors = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLORS_ENABLE) ? TN_STYLE_CUSTOM_COLORS : 0;

    if (treelistCustomColors)
    {
        treelistCreateParams.TextColor = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLOR_TEXT);
        treelistCreateParams.FocusColor = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLOR_FOCUS);
        treelistCreateParams.SelectionColor = PhGetIntegerSetting(SETTING_TREE_LIST_CUSTOM_COLOR_SELECTION);
    }

    HistTreeNewHandle = PhCreateWindow(
        PH_TREENEW_CLASSNAME,
        NULL,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | TN_STYLE_DOUBLE_BUFFERED | thinRows | treelistBorder | treelistCustomColors,
        0,
        0,
        0,
        0,
        HistTabWindowHandle,
        NULL,
        NULL,
        &treelistCreateParams
        );

    if (!HistTreeNewHandle)
        return FALSE;

    if (themeSupport)
    {
        PhInitializeWindowTheme(HistTabWindowHandle, TRUE);
        TreeNew_ThemeSupport(HistTreeNewHandle, TRUE);
    }

    HistInitializeTreeList();
    HistTreeNewCreated = TRUE;
    HistLayout();
    HistTabSnapshotsChanged();

    return TRUE;
}

// Tab page callback

_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
static BOOLEAN NTAPI HistTabPageCallback(
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
            if (!HistCreateTabWindow(Parameter2))
                return FALSE;

            if (Parameter1)
                *(HWND*)Parameter1 = HistTabWindowHandle;
        }
        return TRUE;
    case MainTabPageLoadSettings:
        {
            NOTHING;
        }
        return TRUE;
    case MainTabPageSaveSettings:
        {
            HistSaveTreeListSettings();
        }
        return TRUE;
    case MainTabPageSelected:
        {
            NOTHING;
        }
        break;
    case MainTabPageExportContent:
        {
            PPH_MAIN_TAB_PAGE_EXPORT_CONTENT exportContent = Parameter1;

            if (!HistTreeNewCreated || !exportContent)
                return FALSE;

            HistWriteList(exportContent->FileStream, exportContent->Mode);
        }
        return TRUE;
    case MainTabPageFontChanged:
        {
            if (HistTreeNewHandle)
                SendMessage(HistTreeNewHandle, WM_SETFONT, (WPARAM)Parameter1, TRUE);
        }
        break;
    case MainTabPageDpiChanged:
        {
            if (HistTreeNewCreated)
            {
                HistUpdateFont();
                HistLayout();
            }
        }
        break;
    }

    return FALSE;
}

VOID HistInitializeTab(
    VOID
    )
{
    PH_MAIN_TAB_PAGE page;

    memset(&page, 0, sizeof(PH_MAIN_TAB_PAGE));
    page.Name = HistTabPageText;
    page.Callback = HistTabPageCallback;
    HistTabPage = PhPluginCreateTabPage(&page);
}
