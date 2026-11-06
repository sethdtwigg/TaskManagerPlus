/*
 * TaskManagerPlus: process list grouping ("Apps" / "Background processes") and the totals row.
 *
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 */

#ifndef PH_PROCGROUP_H
#define PH_PROCGROUP_H

#define PH_PROCESS_GROUP_APPS 0
#define PH_PROCESS_GROUP_BACKGROUND 1
#define PH_PROCESS_GROUP_COUNT 2

VOID PhGroupInitialization(
    _In_ HWND TreeNewHandle
    );

BOOLEAN PhGroupEnabled(
    VOID
    );

// Turns grouping on/off (persisted in the GroupProcesses setting) and restructures the list.
VOID PhGroupSetEnabled(
    _In_ BOOLEAN Enabled,
    _In_ PPH_LIST NodeList
    );

BOOLEAN PhGroupIsHeaderNode(
    _In_opt_ PVOID Node
    );

// Reassigns every process node to a group and rebuilds the per-group child lists.
// Returns TRUE if any node changed group.
BOOLEAN PhGroupRebuild(
    _In_ PPH_LIST NodeList
    );

// Per-refresh update: refreshes the window snapshot, rebuilds, restructures on change and
// repaints the totals row.
VOID PhGroupTick(
    _In_ PPH_LIST NodeList
    );

// Child list used for a process node: the in-group children when grouping, else NULL.
PPH_LIST PhGroupGetNodeChildren(
    _In_ PVOID Node
    );

// Supplies children for TreeNewGetChildren while grouping is on. Returns FALSE when grouping is off.
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
    );

// Handles tree messages for header nodes and the totals row. Returns TRUE if handled.
BOOLEAN PhGroupTreeNewCallback(
    _In_ PH_TREENEW_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _Out_ PBOOLEAN Result // the value the tree callback should return when this returns TRUE
    );

// Expands/collapses the group headers. Returns TRUE if any changed.
BOOLEAN PhGroupExpandAll(
    _In_ BOOLEAN Expand
    );

#endif
