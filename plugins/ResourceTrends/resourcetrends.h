/*
 * TaskManagerPlus ResourceTrends plugin.
 *
 * Samples system and per-process CPU / memory / commit on a slow timer, keeps the samples on disk (compact binary
 * file, thinned with age and size-capped), shows sparklines and a "what used CPU/RAM at <time>" lookup on a Trends
 * tab, and flags processes whose private bytes (commit) grow steadily (leak detector).
 */

#ifndef _RESOURCETRENDS_H_
#define _RESOURCETRENDS_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"
#include "trendfmt.h"
#include "leak.h"

#define PLUGIN_NAME L"TaskManagerPlus.ResourceTrends"
#define SETTING_NAME_SAMPLE_SECONDS (PLUGIN_NAME L".SampleSeconds")
#define SETTING_NAME_RETENTION_HOURS (PLUGIN_NAME L".RetentionHours")
#define SETTING_NAME_MAX_FILE_MB (PLUGIN_NAME L".MaxFileMB")
#define SETTING_NAME_PERSIST (PLUGIN_NAME L".PersistToDisk")
#define SETTING_NAME_LEAK_ENABLED (PLUGIN_NAME L".LeakDetector")
#define SETTING_NAME_LEAK_WINDOW_SECONDS (PLUGIN_NAME L".LeakWindowSeconds")
#define SETTING_NAME_LEAK_MIN_GROWTH_MB (PLUGIN_NAME L".LeakMinGrowthMB")
#define SETTING_NAME_LEAK_MIN_PERCENT (PLUGIN_NAME L".LeakMinGrowthPercent")
#define SETTING_NAME_LEAK_NOTIFY (PLUGIN_NAME L".LeakNotify")
#define SETTING_NAME_TEST_OUTPUT (PLUGIN_NAME L".TestOutput") // TaskManagerPlus: path of a text file rewritten after each sample (tests)
#define SETTING_NAME_RANGE_INDEX (PLUGIN_NAME L".RangeIndex")
#define SETTING_NAME_VIEW_INDEX (PLUGIN_NAME L".ViewIndex") // 0 = usage at time, 1 = leak suspects
#define SETTING_NAME_TEST_SELECT_TAB (PLUGIN_NAME L".TestSelectTab") // TaskManagerPlus: 1 = switch to the Trends tab at the first refresh (tests, screenshots)
#define SETTING_NAME_USAGE_COLUMNS (PLUGIN_NAME L".UsageColumns")
#define SETTING_NAME_USAGE_SORT (PLUGIN_NAME L".UsageSort")
#define SETTING_NAME_LEAK_COLUMNS (PLUGIN_NAME L".LeakColumns")
#define SETTING_NAME_LEAK_SORT (PLUGIN_NAME L".LeakSort")

#define TRS_DEFAULT_SAMPLE_SECONDS 30
#define TRS_DEFAULT_RETENTION_HOURS 168
#define TRS_DEFAULT_MAX_FILE_MB 8
#define TRS_DEFAULT_LEAK_WINDOW_SECONDS 1800
#define TRS_DEFAULT_LEAK_MIN_GROWTH_MB 100
#define TRS_DEFAULT_LEAK_MIN_PERCENT 10

#define TRS_FILE_NAME L"tmp-trends.bin"

// Per-sample size limits: the processes stored are the busiest by CPU and the largest by private bytes.
#define TRS_TOP_BY_CPU 24
#define TRS_TOP_BY_MEMORY 24

extern PPH_PLUGIN PluginInstance;

// Store (store.c)

VOID TrsStoreInitialize(VOID);
VOID TrsStoreLoad(VOID);
VOID TrsStoreFlush(VOID);
VOID TrsStoreClear(VOID);

/** Interns the process name (case sensitive) and returns its string index. */
ULONG TrsStoreInternName(_In_ PPH_STRING Name);
PPH_STRING TrsStoreGetName(_In_ ULONG Index);

/** Copies the sample, appends it to memory and file, thins and prunes. */
VOID TrsStoreAddSample(
    _In_ const TR_SYSTEM *System,
    _In_reads_(Count) const TR_PROCESS *Processes,
    _In_ ULONG Count
    );

ULONG TrsStoreGetSampleCount(VOID);
TR_SAMPLE *TrsStoreGetSample(_In_ ULONG Index);
TR_SAMPLE *const *TrsStoreGetSamples(VOID);
ULONG64 TrsStoreGetFileBytes(VOID);

// Collector and leak tracker (tracker.c)

typedef struct _TRS_SUSPECT
{
    ULONG ProcessId; // 0 = system commit
    LONGLONG CreateTime;
    PPH_STRING Name;
    ULONG64 CurrentKB;
    LK_RESULT Result;
} TRS_SUSPECT, *PTRS_SUSPECT;

VOID TrsCollect(VOID);
VOID TrsTrackerShutdown(VOID);
ULONG TrsGetSuspectCount(VOID);
PTRS_SUSPECT TrsGetSuspect(_In_ ULONG Index);

// Seeds the tracker from a stored sample (called while loading the file).
VOID TrsTrackerSeed(
    _In_ const TR_SAMPLE *Sample,
    _In_ BOOLEAN IncludeSystem
    );

ULONG TrsGetSampleSeconds(VOID);
ULONG TrsGetRetentionHours(VOID);
ULONG TrsGetMaxFileMB(VOID);
VOID TrsGetLeakConfig(_Out_ LK_CONFIG *Config);

// Tab (tab.c)

VOID TrsInitializeTab(VOID);
VOID TrsTabDataChanged(VOID);
VOID TrsSelectTab(VOID);

// Options (main.c)

INT_PTR CALLBACK TrsOptionsDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    );

#endif
