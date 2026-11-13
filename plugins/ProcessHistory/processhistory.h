/*
 * TaskManagerPlus Process History plugin.
 *
 * Records periodic snapshots of the process list and lets the user scrub
 * back in time from a "History" main-window tab.
 */

#ifndef _PROCESSHISTORY_H_
#define _PROCESSHISTORY_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"
#include <toolstatusintf.h>
#include <processcaptureintf.h>

#define PLUGIN_NAME L"TaskManagerPlus.ProcessHistory"
#define SETTING_NAME_INTERVAL_SECONDS (PLUGIN_NAME L".IntervalSeconds")
#define SETTING_NAME_RETENTION_HOURS (PLUGIN_NAME L".RetentionHours")
#define SETTING_NAME_PERSIST_TO_DISK (PLUGIN_NAME L".PersistToDisk")
#define SETTING_NAME_SELF_TEST (PLUGIN_NAME L".SelfTest") // TaskManagerPlus: 1 = run the thinning self-test once
#define SETTING_NAME_CAPTURE (PLUGIN_NAME L".CaptureShortLived") // TaskManagerPlus: 1 = ETW capture of short-lived processes (needs admin)
#define SETTING_NAME_CAPTURE_SELF_TEST (PLUGIN_NAME L".CaptureSelfTest") // TaskManagerPlus: 1 = inject a synthetic short-lived process once (tests)
#define SETTING_NAME_TREE_LIST_COLUMNS (PLUGIN_NAME L".TreeListColumns")
#define SETTING_NAME_TREE_LIST_SORT (PLUGIN_NAME L".TreeListSort")

#define HIST_DEFAULT_INTERVAL_SECONDS 10
#define HIST_DEFAULT_RETENTION_HOURS 24
#define HIST_MIN_INTERVAL_SECONDS 1
#define HIST_MAX_INTERVAL_SECONDS 3600
#define HIST_MIN_RETENTION_HOURS 1
#define HIST_MAX_RETENTION_HOURS 720

#define HIST_FILE_NAME L"tmp-history.bin"

extern PPH_PLUGIN PluginInstance;

// Snapshot store (store.c)

#include <pshpack4.h>
/**
 * One process as recorded in a snapshot. Names and paths are indices into the
 * store's string table. The layout is written verbatim to disk, so do not
 * reorder or resize members without bumping HIST_FILE_VERSION.
 */
typedef struct _HIST_ENTRY
{
    ULONG ProcessId;
    ULONG ParentProcessId;
    ULONG SessionId;
    ULONG NameIndex;
    ULONG PathIndex;
    ULONG NumberOfThreads;
    ULONG NumberOfHandles;
    FLOAT CpuUsage;
    LONGLONG CreateTime;
    ULONGLONG PrivateBytes;
    ULONGLONG WorkingSetSize;
} HIST_ENTRY, *PHIST_ENTRY;
#include <poppack.h>

/**
 * A snapshot of the process list at one point in time. Entries are sorted by
 * (ProcessId, CreateTime) so that two snapshots can be diffed by binary search.
 */
typedef struct _HIST_SNAPSHOT
{
    LARGE_INTEGER Time; // system time (UTC)
    ULONG Count;
    ULONG FileBytes; // size of this snapshot's record on disk (0 if never written)
    HIST_ENTRY Entries[ANYSIZE_ARRAY];
} HIST_SNAPSHOT, *PHIST_SNAPSHOT;

VOID HistStoreInitialize(
    VOID
    );

VOID HistStoreLoad(
    VOID
    );

VOID HistStoreFlush(
    VOID
    );

VOID HistStoreCollect(
    VOID
    );

VOID HistStoreClear(
    VOID
    );

ULONG HistStoreGetSnapshotCount(
    VOID
    );

PHIST_SNAPSHOT HistStoreGetSnapshot(
    _In_ ULONG Index
    );

ULONG HistStoreFindNearestIndex(
    _In_ LONGLONG Time
    );

VOID HistStoreSelfTest(
    VOID
    );

PPH_STRING HistStoreGetString(
    _In_ ULONG Index
    );

PHIST_ENTRY HistSnapshotFindEntry(
    _In_ PHIST_SNAPSHOT Snapshot,
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    );

ULONG HistGetIntervalSeconds(
    VOID
    );

ULONG HistGetRetentionHours(
    VOID
    );

// Short-lived process capture (capture.c)

extern PROCESS_CAPTURE_INTERFACE CapInterface;

VOID CapMarkSeen(
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    );

VOID CapUpdate(
    VOID
    );

VOID CapShutdown(
    VOID
    );

// History tab (tab.c)

VOID HistInitializeTab(
    VOID
    );

VOID HistTabSnapshotsChanged(
    VOID
    );

// TaskManagerPlus: time of the snapshot the user is parked on (not in live mode), 0 when none; thinning keeps it.
LONGLONG HistTabGetParkedTime(
    VOID
    );

// Options (main.c)

INT_PTR CALLBACK HistOptionsDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    );

#endif
