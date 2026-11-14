/*
 * TaskManagerPlus TrayTop: building the "Top processes" tray submenu and running its actions.
 *
 * The tray menu is shown modally on the main thread, so the process list captured while it is built stays valid until the
 * item is chosen. The captured process items are kept (referenced) in a small table and released the next time a menu is
 * built, on unload, or once an action has run.
 */

#include "traytop.h"

#define TT_ID_BASE 0x7A00u
#define TT_MAX_ROWS (TT_COUNT_MAX * 2)
#define TT_ACTIONS_PER_ROW 3

typedef struct _TT_ROW
{
    PPH_PROCESS_ITEM Process;
} TT_ROW;

static TT_ROW TtRows[TT_MAX_ROWS];

static ULONG TtMenuId(
    _In_ ULONG Row,
    _In_ TT_ACTION Action
    )
{
    return TT_ID_BASE + Row * TT_ACTIONS_PER_ROW + (Action - 1);
}

VOID TtReleaseMenuReferences(
    VOID
    )
{
    ULONG i;

    for (i = 0; i < TT_MAX_ROWS; i++)
    {
        if (TtRows[i].Process)
        {
            PhDereferenceObject(TtRows[i].Process);
            TtRows[i].Process = NULL;
        }
    }
}

static PPH_STRING TtFormatMemory(
    _In_ ULONG64 Bytes
    )
{
    if (Bytes >= 1024ull * 1024 * 1024)
        return PhFormatString(L"%.1f GB", (double)Bytes / (1024.0 * 1024.0 * 1024.0));

    return PhFormatString(L"%.0f MB", (double)Bytes / (1024.0 * 1024.0));
}

static VOID TtAddList(
    _In_ PPH_EMENU Parent,
    _In_ BOOLEAN ByMemory,
    _In_ PPH_PROCESS_ITEM *Items,
    _In_ TT_ITEM *Summary,
    _In_ ULONG ItemCount,
    _In_ ULONG Count,
    _Inout_ PULONG RowCounter
    )
{
    size_t picked[TT_COUNT_MAX];
    size_t number;
    size_t i;
    PPH_EMENU_ITEM header;

    header = PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_DISABLED, 0, ByMemory ? L"Top by memory" : L"Top by CPU", NULL);
    PhInsertEMenuItem(Parent, header, ULONG_MAX);

    number = TtSelectTop(Summary, ItemCount, ByMemory, Count, picked);

    if (!number)
    {
        PhInsertEMenuItem(Parent, PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_DISABLED, 0, L"  (nothing busy right now)", NULL), ULONG_MAX);
        return;
    }

    for (i = 0; i < number; i++)
    {
        PPH_PROCESS_ITEM process = Items[picked[i]];
        PPH_STRING value;
        PPH_STRING text;
        PPH_EMENU_ITEM row;
        ULONG slot = *RowCounter;

        if (slot >= TT_MAX_ROWS)
            break;

        (*RowCounter)++;
        TtRows[slot].Process = PhReferenceObject(process);

        if (ByMemory)
        {
            value = TtFormatMemory(process->VmCounters.WorkingSetSize);
            text = PhFormatString(L"%s  (%s, PID %lu)", PhGetStringOrDefault(process->ProcessName, L"Unknown"), value->Buffer, HandleToUlong(process->ProcessId));
        }
        else
        {
            value = PhFormatString(L"%.1f%%", (double)process->CpuUsage * 100.0);
            text = PhFormatString(L"%s  (%s, PID %lu)", PhGetStringOrDefault(process->ProcessName, L"Unknown"), value->Buffer, HandleToUlong(process->ProcessId));
        }

        PhDereferenceObject(value);
        // The menu keeps the text pointer, so it gets its own copy and frees it with the item.
        row = PhPluginCreateEMenuItem(PluginInstance, PH_EMENU_TEXT_OWNED, 0, PhDuplicateStringZ(text->Buffer), NULL);
        PhDereferenceObject(text);
        PhInsertEMenuItem(row, PhPluginCreateEMenuItem(PluginInstance, 0, TtMenuId(slot, TtActionEnd), L"End task", NULL), ULONG_MAX);
        PhInsertEMenuItem(row, PhPluginCreateEMenuItem(PluginInstance, 0, TtMenuId(slot, TtActionEfficiency), L"Efficiency mode", NULL), ULONG_MAX);
        PhInsertEMenuItem(row, PhPluginCreateEMenuItem(PluginInstance, 0, TtMenuId(slot, TtActionOpenLocation), L"Open file location", NULL), ULONG_MAX);
        PhInsertEMenuItem(Parent, row, ULONG_MAX);
    }
}

static PPH_EMENU_ITEM TtBuildMenu(
    VOID
    )
{
    PPH_EMENU_ITEM root;
    PPH_PROCESS_ITEM *items;
    ULONG itemCount;
    TT_ITEM *summary;
    ULONG count;
    ULONG rows = 0;
    ULONG i;

    TtReleaseMenuReferences();
    count = TtClampCount((LONG)PhGetIntegerSetting(SETTING_NAME_COUNT));
    root = PhPluginCreateEMenuItem(PluginInstance, 0, 0, L"Top processes", NULL);

    PhEnumProcessItems(&items, &itemCount);
    summary = PhAllocateZero(sizeof(TT_ITEM) * (itemCount + 1));

    for (i = 0; i < itemCount; i++)
    {
        PPH_PROCESS_ITEM process = items[i];

        if (!PH_IS_FAKE_PROCESS_ID(process->ProcessId))
        {
            summary[i].Pid = HandleToUlong(process->ProcessId);
            summary[i].Cpu = (double)process->CpuUsage * 100.0;
            summary[i].Memory = process->VmCounters.WorkingSetSize;
        }
    }

    TtAddList(root, FALSE, items, summary, itemCount, count, &rows);
    PhInsertEMenuItem(root, PhCreateEMenuSeparator(), ULONG_MAX);
    TtAddList(root, TRUE, items, summary, itemCount, count, &rows);

    PhFree(summary);
    PhDereferenceObjects(items, itemCount);
    PhFree(items);
    return root;
}

VOID TtAddToTrayMenu(
    _In_ PPH_PLUGIN_MENU_INFORMATION MenuInfo
    )
{
    PPH_EMENU_ITEM exitItem;
    PPH_EMENU_ITEM root;
    PPH_EMENU_ITEM parent = NULL;
    ULONG index = ULONG_MAX;

    if (!PhGetIntegerSetting(SETTING_NAME_ENABLED))
        return;

    exitItem = PhFindEMenuItem(MenuInfo->Menu, PH_EMENU_FIND_DESCEND, NULL, PHAPP_ID_ICON_EXIT);
    root = TtBuildMenu();

    if (exitItem)
    {
        parent = exitItem->Parent;
        index = parent ? PhIndexOfEMenuItem(parent, exitItem) : ULONG_MAX;
    }

    if (!parent)
        parent = MenuInfo->Menu;

    PhInsertEMenuItem(parent, root, index);
}

BOOLEAN TtRunAction(
    _In_ ULONG Pid,
    _In_ TT_ACTION Action,
    _In_ HWND OwnerWindow
    )
{
    PPH_PROCESS_ITEM process = PhReferenceProcessItem(UlongToHandle(Pid));
    BOOLEAN ok = FALSE;

    if (!process)
        return FALSE;

    switch (Action)
    {
    case TtActionEnd:
        ok = PhUiTerminateProcesses(OwnerWindow, &process, 1); // asks for confirmation
        break;
    case TtActionEfficiency:
        ok = PhUiSetEcoModeProcess(OwnerWindow, process);
        break;
    case TtActionOpenLocation:
        if (process->FileName)
        {
            PPH_STRING fileName = PhGetFileName(process->FileName);

            PhShellExploreFile(OwnerWindow, fileName->Buffer);
            PhDereferenceObject(fileName);
            ok = TRUE;
        }
        break;
    }

    PhDereferenceObject(process);
    return ok;
}

BOOLEAN TtHandleMenuItem(
    _In_ ULONG Id,
    _In_ HWND OwnerWindow
    )
{
    ULONG offset;
    ULONG row;

    if (Id < TT_ID_BASE || Id >= TT_ID_BASE + TT_MAX_ROWS * TT_ACTIONS_PER_ROW)
        return FALSE;

    offset = Id - TT_ID_BASE;
    row = offset / TT_ACTIONS_PER_ROW;

    if (TtRows[row].Process)
        TtRunAction(HandleToUlong(TtRows[row].Process->ProcessId), (TT_ACTION)(offset % TT_ACTIONS_PER_ROW + 1), OwnerWindow);

    return TRUE;
}

VOID TtShowTestMenu(
    _In_ HWND OwnerWindow,
    _In_ LONG X,
    _In_ LONG Y
    )
{
    PPH_EMENU_ITEM root = TtBuildMenu();
    PPH_EMENU_ITEM item;

    // The submenu itself is shown (the lists directly), which is what the tray menu opens when "Top processes" is hovered.
    item = PhShowEMenu(root, OwnerWindow, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, X, Y);

    if (item && item->Id == PHAPP_ID_PLUGIN_MENU_ITEM && item->Context)
        TtHandleMenuItem(((PPH_PLUGIN_MENU_ITEM)item->Context)->Id, OwnerWindow);

    PhDestroyEMenu(root);
}
