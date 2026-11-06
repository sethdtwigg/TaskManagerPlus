/*
 * TaskManagerPlus ProcessRules plugin - evaluation engine
 *
 * Runs once per refresh from GeneralCallbackProcessesUpdated (main thread).
 * Applies on-start actions to newly matched processes, evaluates watchdogs,
 * enforces the runaway guard, and restores processes when rules change or
 * the engine is paused.
 */

#include "processrules.h"

static BOOLEAN TmpEnginePaused = FALSE;
static LARGE_INTEGER TmpEngineStartTime;
static ULONG64 TmpMutedUntil = 0;
static ULONG TmpGlobalGuardCount = 0;
static ULONG64 TmpGlobalGuardWindowStart = 0;
static ULONG TmpDumpCount = 0;
static ULONG64 TmpDumpWindowStart = 0;

#define TMP_GUARD_WINDOW_MS (60 * 1000)
#define TMP_DUMP_WINDOW_MS (60 * 60 * 1000)

VOID TmpEngineInitialize(
    VOID
    )
{
    PhQuerySystemTime(&TmpEngineStartTime);
}

VOID NTAPI TmpProcessItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    memset(Extension, 0, sizeof(TMP_PROCESS_EXTENSION));
}

VOID NTAPI TmpProcessItemDeleteCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PTMP_PROCESS_EXTENSION extension = Extension;

    PhClearReference(&extension->FileNameWin32);
}

BOOLEAN TmpIsEnginePaused(
    VOID
    )
{
    return TmpEnginePaused;
}

BOOLEAN TmpIsMuted(
    VOID
    )
{
    return TmpMutedUntil != 0 && NtGetTickCount64() < TmpMutedUntil;
}

VOID TmpMuteNotifications(
    _In_ ULONG Seconds
    )
{
    TmpMutedUntil = NtGetTickCount64() + (ULONG64)Seconds * 1000;
}

VOID TmpUnmuteNotifications(
    VOID
    )
{
    TmpMutedUntil = 0;
}

static BOOLEAN TmpShouldSkipProcess(
    _In_ PPH_PROCESS_ITEM ProcessItem
    )
{
    if (PH_IS_FAKE_PROCESS_ID(ProcessItem->ProcessId))
        return TRUE;
    if (ProcessItem->ProcessId == SYSTEM_IDLE_PROCESS_ID || ProcessItem->ProcessId == SYSTEM_PROCESS_ID)
        return TRUE;
    if (ProcessItem->ProcessId == NtCurrentProcessId())
        return TRUE;
    if (ProcessItem->IsSecureProcess || ProcessItem->IsProtectedProcess || ProcessItem->IsSecureSystem)
        return TRUE;
    if (!ProcessItem->ProcessName)
        return TRUE;

    return FALSE;
}

/**
 * Records an event on a rule and emits the notification/log entry.
 */
static VOID TmpReportEvent(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _In_ PCWSTR Description,
    _In_ BOOLEAN Important
    )
{
    PPH_STRING text;

    text = PhFormatString(
        L"%s (%lu): %s",
        PhGetStringOrEmpty(ProcessItem->ProcessName),
        HandleToUlong(ProcessItem->ProcessId),
        Description
        );

    Rule->EventCount++;
    PhQuerySystemTime(&Rule->LastEventTime);
    PhMoveReference(&Rule->LastEventText, PhReferenceObject(text));
    PhClearReference(&Rule->DisplayEvents);
    PhClearReference(&Rule->DisplayLastEvent);

    if (PhGetIntegerSetting(SETTING_NAME_LOG))
    {
        PPH_STRING logText = PhConcatStrings2(L"Process rule: ", PhGetString(text));
        PhLogMessageEntry(PH_LOG_ENTRY_MESSAGE, logText);
        PhDereferenceObject(logText);
    }

    if (Important && !Rule->Muted && PhGetIntegerSetting(SETTING_NAME_NOTIFY) && !TmpIsMuted())
    {
        PhShowIconNotification(L"Process rule", PhGetString(text));
    }

    PhDereferenceObject(text);
}

/**
 * Counts an end/suspend action against the global and per-rule runaway guards.
 * Returns FALSE when the action must not run because a guard tripped.
 */
static BOOLEAN TmpCheckRunawayGuard(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _In_ BOOLEAN CountForRule
    )
{
    ULONG64 now = NtGetTickCount64();
    ULONG globalLimit = PhGetIntegerSetting(SETTING_NAME_GUARD_GLOBAL);
    ULONG ruleLimit = PhGetIntegerSetting(SETTING_NAME_GUARD_RULE);

    if (now - TmpGlobalGuardWindowStart > TMP_GUARD_WINDOW_MS)
    {
        TmpGlobalGuardWindowStart = now;
        TmpGlobalGuardCount = 0;
    }

    if (now - Rule->GuardWindowStart > TMP_GUARD_WINDOW_MS)
    {
        Rule->GuardWindowStart = now;
        Rule->GuardCount = 0;
    }

    TmpGlobalGuardCount++;

    if (CountForRule)
        Rule->GuardCount++;

    if (ruleLimit && CountForRule && Rule->GuardCount > ruleLimit)
    {
        Rule->Enabled = FALSE;
        Rule->AutoDisabled = TRUE;
        PhClearReference(&Rule->DisplayOnStart);
        TmpReportEvent(Rule, ProcessItem, L"rule disabled by the runaway guard (too many end actions per minute)", TRUE);
        TmpRulesChanged();
        return FALSE;
    }

    if (globalLimit && TmpGlobalGuardCount > globalLimit)
    {
        TmpReportEvent(Rule, ProcessItem, L"all rules paused by the runaway guard (too many end/suspend actions per minute)", TRUE);
        TmpSetEnginePaused(TRUE);
        return FALSE;
    }

    return TRUE;
}

static VOID TmpSaveOriginalPriority(
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    HANDLE processHandle;
    UCHAR priorityClass;

    if (Extension->PriorityChanged)
        return;

    if (NT_SUCCESS(PhOpenProcess(&processHandle, PROCESS_QUERY_LIMITED_INFORMATION, ProcessItem->ProcessId)))
    {
        if (NT_SUCCESS(PhGetProcessPriorityClass(processHandle, &priorityClass)))
        {
            Extension->OriginalPriority = priorityClass;
            Extension->PriorityChanged = TRUE;
        }

        NtClose(processHandle);
    }
}

static VOID TmpSaveOriginalAffinity(
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    HANDLE processHandle;
    KAFFINITY affinityMask;

    if (Extension->AffinityChanged)
        return;

    if (NT_SUCCESS(PhOpenProcess(&processHandle, PROCESS_QUERY_LIMITED_INFORMATION, ProcessItem->ProcessId)))
    {
        if (NT_SUCCESS(PhGetProcessAffinityMask(processHandle, &affinityMask)))
        {
            Extension->OriginalAffinity = affinityMask;
            Extension->AffinityChanged = TRUE;
        }

        NtClose(processHandle);
    }
}

/**
 * Undoes everything a rule did to a process (priority, affinity, efficiency
 * mode, suspension) and clears its evaluation state.
 */
static VOID TmpRestoreProcess(
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    HANDLE processHandle;

    if (Extension->EfficiencyApplied)
    {
        // Clearing throttling also resets the priority class to normal; restore priority afterwards.
        PhSetProcessItemThrottlingState(ProcessItem, TRUE);
        Extension->EfficiencyApplied = FALSE;
    }

    if (Extension->PriorityChanged)
    {
        PhSetProcessItemPriority(ProcessItem, Extension->OriginalPriority);
        Extension->PriorityChanged = FALSE;
    }

    if (Extension->AffinityChanged)
    {
        PhSetProcessItemAffinityMask(ProcessItem, Extension->OriginalAffinity);
        Extension->AffinityChanged = FALSE;
    }

    if (Extension->SuspendedByRule)
    {
        if (NT_SUCCESS(PhOpenProcess(&processHandle, PROCESS_SUSPEND_RESUME, ProcessItem->ProcessId)))
        {
            PhResumeProcess(processHandle);
            NtClose(processHandle);
        }

        Extension->SuspendedByRule = FALSE;
    }

    Extension->Evaluated = FALSE;
    memset(Extension->SustainedSince, 0, sizeof(Extension->SustainedSince));
    memset(Extension->Fired, 0, sizeof(Extension->Fired));
}

static PPH_STRING TmpExpandCommand(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    PH_STRING_BUILDER sb;
    PH_STRINGREF remaining;
    PH_STRINGREF before;
    PH_STRINGREF token;
    static CONST PH_STRINGREF openBrace = PH_STRINGREF_INIT(L"{");
    static CONST PH_STRINGREF closeBrace = PH_STRINGREF_INIT(L"}");

    if (!Extension->FileNameWin32 && ProcessItem->FileName)
        Extension->FileNameWin32 = PhGetFileName(ProcessItem->FileName);

    PhInitializeStringBuilder(&sb, Rule->Command->Length);
    remaining = Rule->Command->sr;

    while (PhSplitStringRefAtString(&remaining, &openBrace, FALSE, &before, &remaining))
    {
        PhAppendStringBuilder(&sb, &before);

        if (!PhSplitStringRefAtString(&remaining, &closeBrace, FALSE, &token, &remaining))
        {
            PhAppendStringBuilder(&sb, &openBrace);
            PhAppendStringBuilder(&sb, &token);
            remaining.Length = 0;
            break;
        }

        if (PhEqualStringRef2(&token, L"pid", TRUE))
            PhAppendFormatStringBuilder(&sb, L"%lu", HandleToUlong(ProcessItem->ProcessId));
        else if (PhEqualStringRef2(&token, L"name", TRUE))
            PhAppendStringBuilder2(&sb, PhGetStringOrEmpty(ProcessItem->ProcessName));
        else if (PhEqualStringRef2(&token, L"path", TRUE))
            PhAppendStringBuilder2(&sb, PhGetStringOrEmpty(Extension->FileNameWin32));
        else
        {
            PhAppendStringBuilder(&sb, &openBrace);
            PhAppendStringBuilder(&sb, &token);
            PhAppendStringBuilder(&sb, &closeBrace);
        }
    }

    PhAppendStringBuilder(&sb, &remaining);

    return PhFinalStringBuilderString(&sb);
}

static VOID TmpRunCommand(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    NTSTATUS status;
    PPH_STRING commandLine;

    if (PhIsNullOrEmptyString(Rule->Command))
        return;

    commandLine = TmpExpandCommand(Rule, ProcessItem, Extension);

    status = PhCreateProcessWin32(
        NULL,
        PhGetString(commandLine),
        NULL,
        NULL,
        PH_CREATE_PROCESS_NEW_CONSOLE,
        NULL,
        NULL,
        NULL
        );

    if (NT_SUCCESS(status))
    {
        TmpReportEvent(Rule, ProcessItem, L"command started", FALSE);
    }
    else
    {
        PPH_STRING message = PhFormatString(L"unable to start the command (0x%x)", status);
        TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
        PhDereferenceObject(message);
    }

    PhDereferenceObject(commandLine);
}

static BOOLEAN TmpEndProcess(
    _In_ PPH_PROCESS_ITEM ProcessItem
    )
{
    NTSTATUS status;
    HANDLE processHandle;

    status = PhOpenProcess(&processHandle, PROCESS_TERMINATE, ProcessItem->ProcessId);

    if (NT_SUCCESS(status))
    {
        status = PhTerminateProcess(processHandle, STATUS_SUCCESS);
        NtClose(processHandle);
    }

    return NT_SUCCESS(status);
}

static PPH_STRING TmpGetDumpDirectory(
    VOID
    )
{
    PPH_STRING directory;
    static CONST PH_STRINGREF defaultPath = PH_STRINGREF_INIT(L"\\TaskManagerPlus\\Dumps");

    directory = PhGetStringSetting(SETTING_NAME_DUMP_DIRECTORY);

    if (PhIsNullOrEmptyString(directory))
    {
        PhClearReference(&directory);
        directory = PhGetKnownLocation(PH_FOLDERID_LocalAppData, &defaultPath, FALSE);
    }

    return directory;
}

static VOID TmpWriteDump(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem
    )
{
    NTSTATUS status;
    ULONG64 now = NtGetTickCount64();
    ULONG limit = PhGetIntegerSetting(SETTING_NAME_DUMPS_PER_HOUR);
    PPH_STRING directory;
    PPH_STRING fileName;
    SYSTEMTIME systemTime;
    LARGE_INTEGER time;
    HANDLE processHandle;
    HANDLE fileHandle;

    if (now - TmpDumpWindowStart > TMP_DUMP_WINDOW_MS)
    {
        TmpDumpWindowStart = now;
        TmpDumpCount = 0;
    }

    if (limit && TmpDumpCount >= limit)
    {
        TmpReportEvent(Rule, ProcessItem, L"dump skipped (hourly dump limit reached)", TRUE);
        return;
    }

    TmpDumpCount++;

    if (!(directory = TmpGetDumpDirectory()))
    {
        TmpReportEvent(Rule, ProcessItem, L"dump skipped (no dump directory)", TRUE);
        return;
    }

    PhCreateDirectoryFullPathWin32(&directory->sr);

    PhQuerySystemTime(&time);
    PhLargeIntegerToLocalSystemTime(&systemTime, &time);

    fileName = PhFormatString(
        L"%s\\%s_%lu_%04u%02u%02u-%02u%02u%02u.dmp",
        PhGetString(directory),
        PhGetStringOrEmpty(ProcessItem->ProcessName),
        HandleToUlong(ProcessItem->ProcessId),
        systemTime.wYear, systemTime.wMonth, systemTime.wDay,
        systemTime.wHour, systemTime.wMinute, systemTime.wSecond
        );

    status = PhOpenProcess(
        &processHandle,
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE,
        ProcessItem->ProcessId
        );

    if (NT_SUCCESS(status))
    {
        status = PhCreateFileWin32(
            &fileHandle,
            PhGetString(fileName),
            FILE_GENERIC_WRITE,
            FILE_ATTRIBUTE_NORMAL,
            0,
            FILE_OVERWRITE_IF,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
            );

        if (NT_SUCCESS(status))
        {
            if (!PhWriteMiniDumpProcess(
                processHandle,
                ProcessItem->ProcessId,
                fileHandle,
                MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo,
                NULL,
                NULL,
                NULL
                ))
            {
                status = PhGetLastWin32ErrorAsNtStatus();
            }

            NtClose(fileHandle);
        }

        NtClose(processHandle);
    }

    if (NT_SUCCESS(status))
    {
        PPH_STRING message = PhConcatStrings2(L"dump written to ", PhGetString(fileName));
        TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
        PhDereferenceObject(message);
    }
    else
    {
        PPH_STRING message = PhFormatString(L"unable to write dump (0x%x)", status);
        TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
        PhDereferenceObject(message);
    }

    PhDereferenceObject(fileName);
    PhDereferenceObject(directory);
}

/**
 * Applies a rule's on-start actions. Returns TRUE when the process was ended.
 */
static BOOLEAN TmpApplyStartActions(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    BOOLEAN newProcess;
    KAFFINITY affinityMask;

    // Processes that existed before the engine started are not blocked or used
    // to run commands; they only get priority/affinity/efficiency adjustments.
    newProcess = ProcessItem->CreateTime.QuadPart > TmpEngineStartTime.QuadPart;

    if (Rule->StartEnd && newProcess)
    {
        if (TmpCheckRunawayGuard(Rule, ProcessItem, TRUE))
        {
            if (TmpEndProcess(ProcessItem))
                TmpReportEvent(Rule, ProcessItem, L"ended on start (blocked)", TRUE);
            else
                TmpReportEvent(Rule, ProcessItem, L"unable to end the process on start", TRUE);
        }

        return TRUE;
    }

    if (Rule->StartPriority)
    {
        TmpSaveOriginalPriority(ProcessItem, Extension);
        PhSetProcessItemPriority(ProcessItem, (UCHAR)Rule->StartPriority);
    }

    if (Rule->StartAffinity != TmpAffinityNone && (affinityMask = TmpGetAffinityPresetMask(Rule)))
    {
        TmpSaveOriginalAffinity(ProcessItem, Extension);
        PhSetProcessItemAffinityMask(ProcessItem, affinityMask);
    }

    if (Rule->StartEfficiencyMode)
    {
        if (NT_SUCCESS(PhSetProcessItemThrottlingState(ProcessItem, FALSE)))
            Extension->EfficiencyApplied = TRUE;
    }

    if (newProcess && !Extension->CommandRan && !PhIsNullOrEmptyString(Rule->Command))
    {
        Extension->CommandRan = TRUE;
        TmpRunCommand(Rule, ProcessItem, Extension);
    }

    return FALSE;
}

static ULONG64 TmpGetMetricValue(
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _In_ TMP_METRIC Metric,
    _In_ ULONG IntervalMs
    )
{
    switch (Metric)
    {
    case TmpMetricCpu:
        return (ULONG64)(ProcessItem->CpuUsage * 100.0f);
    case TmpMetricPrivateBytes:
        return ProcessItem->VmCounters.PagefileUsage;
    case TmpMetricWorkingSet:
        return ProcessItem->VmCounters.WorkingSetSize;
    case TmpMetricIoRate:
        {
            ULONG64 bytes = ProcessItem->IoReadDelta.Delta + ProcessItem->IoWriteDelta.Delta + ProcessItem->IoOtherDelta.Delta;
            return IntervalMs ? bytes * 1000 / IntervalMs : bytes;
        }
    case TmpMetricHandles:
        return ProcessItem->NumberOfHandles;
    case TmpMetricThreads:
        return ProcessItem->NumberOfThreads;
    }

    return 0;
}

/**
 * Runs a watchdog action. Returns TRUE when the process was ended.
 */
static BOOLEAN TmpExecuteWatchdogAction(
    _In_ PTMP_RULE Rule,
    _In_ PTMP_WATCHDOG Watchdog,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    PPH_STRING condition;
    PPH_STRING message;
    HANDLE processHandle;

    condition = TmpFormatWatchdog(Watchdog);

    switch (Watchdog->Action)
    {
    case TmpActionNotify:
        TmpReportEvent(Rule, ProcessItem, PhGetString(condition), TRUE);
        break;
    case TmpActionLog:
        TmpReportEvent(Rule, ProcessItem, PhGetString(condition), FALSE);
        break;
    case TmpActionLowerPriority:
        {
            UCHAR newPriority;

            TmpSaveOriginalPriority(ProcessItem, Extension);

            switch (ProcessItem->PriorityClass)
            {
            case PROCESS_PRIORITY_CLASS_REALTIME:
            case PROCESS_PRIORITY_CLASS_HIGH:
            case PROCESS_PRIORITY_CLASS_ABOVE_NORMAL:
                newPriority = PROCESS_PRIORITY_CLASS_NORMAL;
                break;
            case PROCESS_PRIORITY_CLASS_NORMAL:
                newPriority = PROCESS_PRIORITY_CLASS_BELOW_NORMAL;
                break;
            default:
                newPriority = PROCESS_PRIORITY_CLASS_IDLE;
                break;
            }

            PhSetProcessItemPriority(ProcessItem, newPriority);
            message = PhFormatString(L"%s -> priority lowered to %s", PhGetString(condition), TmpGetPriorityName(newPriority));
            TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
            PhDereferenceObject(message);
        }
        break;
    case TmpActionEfficiencyMode:
        {
            if (NT_SUCCESS(PhSetProcessItemThrottlingState(ProcessItem, FALSE)))
                Extension->EfficiencyApplied = TRUE;

            message = PhConcatStrings2(PhGetString(condition), L" -> efficiency mode enabled");
            TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
            PhDereferenceObject(message);
        }
        break;
    case TmpActionSuspend:
        {
            if (Extension->SuspendedByRule)
                break;
            if (!TmpCheckRunawayGuard(Rule, ProcessItem, FALSE))
                break;

            if (NT_SUCCESS(PhOpenProcess(&processHandle, PROCESS_SUSPEND_RESUME, ProcessItem->ProcessId)))
            {
                if (NT_SUCCESS(PhSuspendProcess(processHandle)))
                    Extension->SuspendedByRule = TRUE;

                NtClose(processHandle);
            }

            message = PhConcatStrings2(PhGetString(condition), Extension->SuspendedByRule ? L" -> suspended" : L" -> unable to suspend");
            TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
            PhDereferenceObject(message);
        }
        break;
    case TmpActionEnd:
        {
            BOOLEAN ended = FALSE;

            if (!TmpCheckRunawayGuard(Rule, ProcessItem, TRUE))
                break;

            ended = TmpEndProcess(ProcessItem);
            message = PhConcatStrings2(PhGetString(condition), ended ? L" -> ended" : L" -> unable to end");
            TmpReportEvent(Rule, ProcessItem, PhGetString(message), TRUE);
            PhDereferenceObject(message);
            PhDereferenceObject(condition);
            return ended;
        }
    case TmpActionDump:
        TmpWriteDump(Rule, ProcessItem);
        break;
    case TmpActionRunCommand:
        TmpRunCommand(Rule, ProcessItem, Extension);
        break;
    }

    PhDereferenceObject(condition);
    return FALSE;
}

/**
 * Evaluates a rule's watchdogs against one process. Returns TRUE when the process was ended.
 */
static BOOLEAN TmpEvaluateWatchdogs(
    _In_ PTMP_RULE Rule,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension,
    _In_ ULONG IntervalMs,
    _In_ ULONG64 Now
    )
{
    for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
    {
        PTMP_WATCHDOG watchdog = &Rule->Watchdogs[i];
        ULONG64 value;

        if (!watchdog->Enabled)
            continue;

        value = TmpGetMetricValue(ProcessItem, watchdog->Metric, IntervalMs);

        if (value > watchdog->Threshold)
        {
            if (Extension->SustainedSince[i] == 0)
            {
                Extension->SustainedSince[i] = Now ? Now : 1;
            }
            else if (!Extension->Fired[i] && Now - Extension->SustainedSince[i] >= (ULONG64)watchdog->DurationSeconds * 1000)
            {
                Extension->Fired[i] = TRUE;

                if (TmpExecuteWatchdogAction(Rule, watchdog, ProcessItem, Extension))
                    return TRUE;

                if (!Rule->Enabled || TmpEnginePaused)
                    return FALSE;
            }
        }
        else
        {
            // Condition cleared; the watchdog may fire again next time it holds.
            Extension->SustainedSince[i] = 0;
            Extension->Fired[i] = FALSE;
        }
    }

    return FALSE;
}

/**
 * Pauses or resumes the engine. Pausing restores every process a rule touched;
 * resuming re-applies on-start actions (block/command excluded for existing processes).
 */
VOID TmpSetEnginePaused(
    _In_ BOOLEAN Paused
    )
{
    PPH_PROCESS_ITEM* processes;
    ULONG numberOfProcesses;

    if (TmpEnginePaused == Paused)
        return;

    TmpEnginePaused = Paused;

    if (Paused)
    {
        PhEnumProcessItems(&processes, &numberOfProcesses);

        for (ULONG i = 0; i < numberOfProcesses; i++)
        {
            PTMP_PROCESS_EXTENSION extension = PhPluginGetObjectExtension(PluginInstance, processes[i], EmProcessItemType);

            if (extension->RuleId)
                TmpRestoreProcess(processes[i], extension);
        }

        PhDereferenceObjects(processes, numberOfProcesses);
        PhFree(processes);
    }
    else
    {
        TmpRulesVersion++; // forces re-evaluation on the next refresh
    }

    TmpRefreshRulesTab();
}

VOID NTAPI TmpProcessesUpdatedCallback(
    _In_ PVOID Parameter,
    _In_ PVOID Context
    )
{
    PPH_PROCESS_ITEM* processes;
    ULONG numberOfProcesses;
    ULONG intervalMs;
    ULONG64 now;
    ULONG64 eventsBefore = 0;
    ULONG64 eventsAfter = 0;

    if (!PhGetIntegerSetting(SETTING_NAME_ENGINE_ENABLED))
        return;

    intervalMs = PhGetIntegerSetting(L"UpdateInterval");
    now = NtGetTickCount64();

    if (TmpRulesList)
    {
        for (ULONG i = 0; i < TmpRulesList->Count; i++)
            eventsBefore += ((PTMP_RULE)TmpRulesList->Items[i])->EventCount;
    }

    PhEnumProcessItems(&processes, &numberOfProcesses);

    for (ULONG i = 0; i < numberOfProcesses; i++)
    {
        PPH_PROCESS_ITEM processItem = processes[i];
        PTMP_PROCESS_EXTENSION extension;
        PTMP_RULE rule;

        if (TmpShouldSkipProcess(processItem))
            continue;

        extension = PhPluginGetObjectExtension(PluginInstance, processItem, EmProcessItemType);

        if (extension->RulesVersion != TmpRulesVersion)
        {
            ULONG newRuleId;

            rule = TmpFindRuleForProcess(processItem, extension);
            newRuleId = rule ? rule->Id : 0;

            // Rules changed: undo what the previous rule did, then re-evaluate.
            if (extension->Evaluated || extension->RuleId != newRuleId)
                TmpRestoreProcess(processItem, extension);

            extension->RuleId = newRuleId;
            extension->RulesVersion = TmpRulesVersion;
        }

        if (!extension->RuleId || TmpEnginePaused)
            continue;

        rule = TmpFindRuleById(extension->RuleId);

        if (!rule || !rule->Enabled)
            continue;

        if (!extension->Evaluated)
        {
            extension->Evaluated = TRUE;

            if (TmpApplyStartActions(rule, processItem, extension))
                continue;
            if (TmpEnginePaused)
                break;
        }

        if (TmpEvaluateWatchdogs(rule, processItem, extension, intervalMs, now))
            continue;
        if (TmpEnginePaused)
            break;
    }

    PhDereferenceObjects(processes, numberOfProcesses);
    PhFree(processes);

    if (TmpRulesList)
    {
        for (ULONG i = 0; i < TmpRulesList->Count; i++)
            eventsAfter += ((PTMP_RULE)TmpRulesList->Items[i])->EventCount;
    }

    if (eventsAfter != eventsBefore)
        TmpRefreshRulesTab();
}
