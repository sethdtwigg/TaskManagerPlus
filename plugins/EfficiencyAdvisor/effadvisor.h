/*
 * TaskManagerPlus EfficiencyAdvisor plugin.
 *
 * An "Advisor" tab that lists the top background offenders (sustained CPU, memory or disk use while the program is not the
 * foreground application) with one-click Efficiency mode, lower priority and "make a Process Rules rule". Nothing is ever
 * done without a click. Critical and system processes, protected processes, services accounts and System Informer itself
 * are never listed.
 */

#ifndef _EFFADVISOR_H_
#define _EFFADVISOR_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"
#include <processrulesintf.h>
#include <commandsintf.h>
#include "advisor.h"

#define PLUGIN_NAME TMP_COMMANDS_ADVISOR_PLUGIN_NAME
#define SETTING_NAME_MIN_BACKGROUND_SECONDS (PLUGIN_NAME L".MinBackgroundSeconds")
#define SETTING_NAME_HALF_LIFE_SECONDS (PLUGIN_NAME L".HalfLifeSeconds")
#define SETTING_NAME_CPU_THRESHOLD_TENTHS (PLUGIN_NAME L".CpuThresholdTenthsPercent") // of the whole system, tenths of a percent
#define SETTING_NAME_MEMORY_THRESHOLD_MB (PLUGIN_NAME L".MemoryThresholdMB")
#define SETTING_NAME_IO_THRESHOLD_KBPS (PLUGIN_NAME L".IoThresholdKBps")
#define SETTING_NAME_MIN_SCORE_TENTHS (PLUGIN_NAME L".MinScoreTenths")
#define SETTING_NAME_IGNORED (PLUGIN_NAME L".IgnoredPrograms") // newline separated image names
#define SETTING_NAME_COLUMNS (PLUGIN_NAME L".TreeListColumns")
#define SETTING_NAME_SORT (PLUGIN_NAME L".TreeListSort")
#define SETTING_NAME_TEST_OUTPUT (PLUGIN_NAME L".TestOutput") // TaskManagerPlus: ranking written to this text file at every rebuild (tests)
#define SETTING_NAME_TEST_PROGRAM (PLUGIN_NAME L".TestProgram") // TaskManagerPlus: image name the test message ops act on
#define SETTING_NAME_TEST_ALLOW_NO_FOREGROUND (PLUGIN_NAME L".TestAllowNoForeground") // TaskManagerPlus: 1 = keep sampling when there is no foreground window (a hidden desktop has none)
#define COMMAND_MESSAGE_NAME L"TaskManagerPlus.EfficiencyAdvisor.Command" // wParam 1 select tab, 2 efficiency, 3 lower priority, 4 ignore, 5 rebuild now

#define EA_COMMAND_SHOW 1 // palette command id

#define EA_REBUILD_INTERVAL_MS 3000

typedef struct _EA_ROW
{
    PH_TREENEW_NODE Node; // first
    ULONG ProcessId;
    LONGLONG CreateTime;
    PPH_STRING Name;
    double Score;
    AD_REASON Reason;
    AD_STATS Stats;
    BOOLEAN EfficiencyMode;
    ULONG PriorityClass;

    PPH_STRING Why;
    PPH_STRING Cpu;
    PPH_STRING Memory;
    PPH_STRING Disk;
    PPH_STRING Background;
    PPH_STRING Efficiency;
    PPH_STRING Priority;
    PPH_STRING ScoreText;
    PPH_STRING PidText;
} EA_ROW, *PEA_ROW;

typedef struct _EA_EXTENSION
{
    AD_STATS Stats;
    BOOLEAN Excluded; // critical / system / ignored: never sampled for the list
} EA_EXTENSION, *PEA_EXTENSION;

extern PPH_PLUGIN PluginInstance;
extern TMP_COMMANDS_INTERFACE EaCommandsInterface;

// tracker.c

VOID EaTrackerInitialize(
    VOID
    );

/** Once per refresh (main thread): foreground detection and one sample for every process. */
VOID EaSample(
    VOID
    );

VOID EaGetConfig(
    _Out_ AD_CONFIG *Config
    );

/** Builds the ranked row list (free with EaFreeRows). Also writes the test output file when the setting is set. */
PPH_LIST EaBuildRows(
    VOID
    );

VOID EaFreeRows(
    _In_ PPH_LIST Rows
    );

ULONG EaGetWatchedCount(
    VOID
    );

VOID EaAddIgnored(
    _In_ PPH_STRING Name
    );

// tab.c

VOID EaInitializeTab(
    VOID
    );

VOID EaSelectTab(
    VOID
    );

/** Rebuilds when the interval has passed (or Force); updates the tab when it exists. */
VOID EaRebuild(
    _In_ BOOLEAN Force
    );

/** Test seam: acts on the row of the TestProgram setting like the buttons. Op: 2 efficiency, 3 lower priority, 4 ignore. */
VOID EaTestAction(
    _In_ ULONG Op
    );

#endif
