/*
 * TaskManagerPlus ProcessRules plugin
 *
 * Per-program rules: actions applied when a program starts (priority,
 * affinity preset, efficiency mode, block, run command) plus up to four
 * watchdogs per rule (sustained metric threshold -> action).
 */

#ifndef PROCESSRULES_H
#define PROCESSRULES_H

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include <json.h>
#include <symprv.h>

#include "resource.h"

#define PLUGIN_NAME L"TaskManagerPlus.ProcessRules"
#define SETTING_NAME_RULES_COLUMNS (PLUGIN_NAME L".RulesColumns")
#define SETTING_NAME_RULES_SORT (PLUGIN_NAME L".RulesSort")
#define SETTING_NAME_ENGINE_ENABLED (PLUGIN_NAME L".EngineEnabled")
#define SETTING_NAME_NOTIFY (PLUGIN_NAME L".Notify")
#define SETTING_NAME_LOG (PLUGIN_NAME L".Log")
#define SETTING_NAME_GUARD_GLOBAL (PLUGIN_NAME L".GuardGlobalPerMinute")
#define SETTING_NAME_GUARD_RULE (PLUGIN_NAME L".GuardRulePerMinute")
#define SETTING_NAME_DUMPS_PER_HOUR (PLUGIN_NAME L".DumpsPerHour")
#define SETTING_NAME_DUMP_DIRECTORY (PLUGIN_NAME L".DumpDirectory")

#define TMP_RULES_FILE_NAME L"tmp-rules.json"
#define TMP_RULES_FILE_VERSION 1
#define TMP_MAX_RULES 128
#define TMP_MAX_WATCHDOGS 4
#define TMP_MUTE_SECONDS (60 * 60)

// Menu item ids (plugin callback)
#define TMP_MENU_CREATE_RULE 1
#define TMP_MENU_EDIT_RULE 2
#define TMP_MENU_TOGGLE_RULE 3

typedef enum _TMP_AFFINITY_PRESET
{
    TmpAffinityNone = 0,
    TmpAffinityAll,
    TmpAffinityPCores,
    TmpAffinityECores,
    TmpAffinityCustom,
    TmpAffinityMaximum
} TMP_AFFINITY_PRESET;

typedef enum _TMP_METRIC
{
    TmpMetricCpu = 0,       // percent
    TmpMetricPrivateBytes,  // bytes
    TmpMetricWorkingSet,    // bytes
    TmpMetricIoRate,        // bytes per second
    TmpMetricHandles,       // count
    TmpMetricThreads,       // count
    TmpMetricMaximum
} TMP_METRIC;

typedef enum _TMP_ACTION
{
    TmpActionNotify = 0,
    TmpActionLog,
    TmpActionLowerPriority,
    TmpActionEfficiencyMode,
    TmpActionSuspend,
    TmpActionEnd,
    TmpActionDump,
    TmpActionRunCommand,
    TmpActionMaximum
} TMP_ACTION;

typedef struct _TMP_WATCHDOG
{
    BOOLEAN Enabled;
    TMP_METRIC Metric;
    ULONG64 Threshold;
    ULONG DurationSeconds;
    TMP_ACTION Action;
} TMP_WATCHDOG, *PTMP_WATCHDOG;

typedef struct _TMP_RULE
{
    PH_TREENEW_NODE Node; // must be first (Rules tab)

    ULONG Id;
    BOOLEAN Enabled;
    BOOLEAN AutoDisabled; // disabled by the runaway guard
    PPH_STRING ProgramName; // image base name, e.g. chrome.exe
    PPH_STRING MatchPath; // optional: full path must contain this text

    // Applied when a matching process starts
    ULONG StartPriority; // 0 = no change, else PROCESS_PRIORITY_CLASS_*
    TMP_AFFINITY_PRESET StartAffinity;
    KAFFINITY StartAffinityMask; // TmpAffinityCustom
    BOOLEAN StartEfficiencyMode;
    BOOLEAN StartEnd; // block: end the process immediately
    PPH_STRING Command; // run command (on start and TmpActionRunCommand)

    TMP_WATCHDOG Watchdogs[TMP_MAX_WATCHDOGS];

    // Runtime state (not persisted)
    ULONG64 EventCount;
    LARGE_INTEGER LastEventTime;
    PPH_STRING LastEventText;
    ULONG GuardCount;
    ULONG64 GuardWindowStart;

    // Cached display strings (Rules tab)
    PPH_STRING DisplayWatchdogs;
    PPH_STRING DisplayOnStart;
    PPH_STRING DisplayEvents;
    PPH_STRING DisplayLastEvent;
} TMP_RULE, *PTMP_RULE;

typedef struct _TMP_PROCESS_EXTENSION
{
    ULONG RulesVersion;
    ULONG RuleId; // 0 = no matching rule
    BOOLEAN Evaluated; // on-start actions applied
    BOOLEAN CommandRan;
    BOOLEAN PriorityChanged;
    BOOLEAN AffinityChanged;
    BOOLEAN EfficiencyApplied;
    BOOLEAN SuspendedByRule;
    UCHAR OriginalPriority;
    KAFFINITY OriginalAffinity;
    PPH_STRING FileNameWin32;
    ULONG64 SustainedSince[TMP_MAX_WATCHDOGS]; // tick count, 0 = condition not met
    BOOLEAN Fired[TMP_MAX_WATCHDOGS];
} TMP_PROCESS_EXTENSION, *PTMP_PROCESS_EXTENSION;

// main.c

extern PPH_PLUGIN PluginInstance;

// rules.c

extern PPH_LIST TmpRulesList;
extern ULONG TmpRulesVersion;

PTMP_RULE TmpCreateRule(
    VOID
    );

VOID TmpDeleteRule(
    _In_ PTMP_RULE Rule
    );

VOID TmpCopyRule(
    _Inout_ PTMP_RULE Destination,
    _In_ PTMP_RULE Source
    );

VOID TmpAddRule(
    _In_ PTMP_RULE Rule
    );

VOID TmpRemoveRule(
    _In_ PTMP_RULE Rule
    );

PTMP_RULE TmpFindRuleById(
    _In_ ULONG Id
    );

PTMP_RULE TmpFindRuleByProgramName(
    _In_ PCPH_STRINGREF ProgramName
    );

PTMP_RULE TmpFindRuleForProcess(
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    );

VOID TmpRulesChanged(
    VOID
    );

VOID TmpInvalidateRuleDisplay(
    _In_ PTMP_RULE Rule
    );

PPH_STRING TmpGetRulesFileName(
    VOID
    );

NTSTATUS TmpSaveRulesToFile(
    _In_ PCPH_STRINGREF FileName
    );

NTSTATUS TmpLoadRulesFromFile(
    _In_ PCPH_STRINGREF FileName,
    _In_ BOOLEAN Merge
    );

VOID TmpLoadRules(
    VOID
    );

VOID TmpSaveRules(
    VOID
    );

BOOLEAN TmpIsHybridSystem(
    VOID
    );

KAFFINITY TmpGetCoreMask(
    _In_ BOOLEAN EfficientCores
    );

KAFFINITY TmpGetAffinityPresetMask(
    _In_ PTMP_RULE Rule
    );

PCWSTR TmpGetMetricName(
    _In_ TMP_METRIC Metric
    );

PCWSTR TmpGetMetricUnit(
    _In_ TMP_METRIC Metric
    );

PCWSTR TmpGetActionName(
    _In_ TMP_ACTION Action
    );

PCWSTR TmpGetPriorityName(
    _In_ ULONG PriorityClass
    );

PCWSTR TmpGetAffinityPresetName(
    _In_ TMP_AFFINITY_PRESET Preset
    );

PPH_STRING TmpFormatWatchdog(
    _In_ PTMP_WATCHDOG Watchdog
    );

PPH_STRING TmpFormatOnStart(
    _In_ PTMP_RULE Rule
    );

// engine.c

VOID TmpEngineInitialize(
    VOID
    );

VOID NTAPI TmpProcessesUpdatedCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    );

VOID NTAPI TmpProcessItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    );

VOID NTAPI TmpProcessItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    );

BOOLEAN TmpIsEnginePaused(
    VOID
    );

VOID TmpSetEnginePaused(
    _In_ BOOLEAN Paused
    );

BOOLEAN TmpIsMuted(
    VOID
    );

VOID TmpMuteNotifications(
    _In_ ULONG Seconds
    );

VOID TmpUnmuteNotifications(
    VOID
    );

// ruledlg.c

BOOLEAN TmpShowRuleDialog(
    _In_ HWND ParentWindowHandle,
    _Inout_ PTMP_RULE Rule
    );

// tab.c

VOID TmpInitializeRulesTab(
    VOID
    );

VOID TmpRefreshRulesTab(
    VOID
    );

VOID TmpEditRuleForProcess(
    _In_ HWND ParentWindowHandle,
    _In_ PPH_PROCESS_ITEM ProcessItem
    );

// options.c

INT_PTR CALLBACK TmpOptionsDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    );

#endif
