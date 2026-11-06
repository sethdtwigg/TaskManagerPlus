/*
 * TaskManagerPlus ProcessRules plugin - rule model, persistence, core masks
 *
 * All rule list access happens on the main thread (engine callback, tab,
 * dialogs), so no locking is needed.
 */

#include "processrules.h"

PPH_LIST TmpRulesList = NULL;
ULONG TmpRulesVersion = 1;
static ULONG TmpNextRuleId = 1;

static BOOLEAN TmpCoreMasksInitialized = FALSE;
static KAFFINITY TmpPCoreMask = 0;
static KAFFINITY TmpECoreMask = 0;
static KAFFINITY TmpAllCoreMask = 0;

static CONST PH_STRINGREF TmpRulesFileNameSr = PH_STRINGREF_INIT(TMP_RULES_FILE_NAME);
static CONST PH_STRINGREF TmpRulesRoamingPathSr = PH_STRINGREF_INIT(L"\\SystemInformer\\" TMP_RULES_FILE_NAME);

PTMP_RULE TmpCreateRule(
    VOID
    )
{
    PTMP_RULE rule;

    rule = PhAllocateZero(sizeof(TMP_RULE));
    PhInitializeTreeNewNode(&rule->Node);
    rule->Id = TmpNextRuleId++;
    rule->Enabled = TRUE;
    rule->ProgramName = PhReferenceEmptyString();
    rule->MatchPath = PhReferenceEmptyString();
    rule->Command = PhReferenceEmptyString();

    for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
    {
        rule->Watchdogs[i].Metric = TmpMetricCpu;
        rule->Watchdogs[i].Threshold = 80;
        rule->Watchdogs[i].DurationSeconds = 30;
        rule->Watchdogs[i].Action = TmpActionNotify;
    }

    return rule;
}

VOID TmpInvalidateRuleDisplay(
    _In_ PTMP_RULE Rule
    )
{
    PhClearReference(&Rule->DisplayWatchdogs);
    PhClearReference(&Rule->DisplayOnStart);
    PhClearReference(&Rule->DisplayEvents);
    PhClearReference(&Rule->DisplayLastEvent);
}

VOID TmpDeleteRule(
    _In_ PTMP_RULE Rule
    )
{
    TmpInvalidateRuleDisplay(Rule);
    PhClearReference(&Rule->ProgramName);
    PhClearReference(&Rule->MatchPath);
    PhClearReference(&Rule->Command);
    PhClearReference(&Rule->LastEventText);
    PhFree(Rule);
}

/**
 * Copies the configuration (not the id, node or runtime state) of one rule into another.
 */
VOID TmpCopyRule(
    _Inout_ PTMP_RULE Destination,
    _In_ PTMP_RULE Source
    )
{
    Destination->Enabled = Source->Enabled;
    Destination->Muted = Source->Muted;
    PhSwapReference(&Destination->ProgramName, Source->ProgramName);
    PhSwapReference(&Destination->MatchPath, Source->MatchPath);
    PhSwapReference(&Destination->Command, Source->Command);
    Destination->StartPriority = Source->StartPriority;
    Destination->StartAffinity = Source->StartAffinity;
    Destination->StartAffinityMask = Source->StartAffinityMask;
    Destination->StartEfficiencyMode = Source->StartEfficiencyMode;
    Destination->StartEnd = Source->StartEnd;
    memcpy(Destination->Watchdogs, Source->Watchdogs, sizeof(Source->Watchdogs));
    TmpInvalidateRuleDisplay(Destination);
}

VOID TmpAddRule(
    _In_ PTMP_RULE Rule
    )
{
    if (!TmpRulesList)
        TmpRulesList = PhCreateList(16);

    PhAddItemList(TmpRulesList, Rule);
}

VOID TmpRemoveRule(
    _In_ PTMP_RULE Rule
    )
{
    ULONG index;

    if (!TmpRulesList)
        return;

    index = PhFindItemList(TmpRulesList, Rule);

    if (index != ULONG_MAX)
        PhRemoveItemList(TmpRulesList, index);
}

PTMP_RULE TmpFindRuleById(
    _In_ ULONG Id
    )
{
    if (!TmpRulesList || Id == 0)
        return NULL;

    for (ULONG i = 0; i < TmpRulesList->Count; i++)
    {
        PTMP_RULE rule = TmpRulesList->Items[i];

        if (rule->Id == Id)
            return rule;
    }

    return NULL;
}

PTMP_RULE TmpFindRuleByProgramName(
    _In_ PCPH_STRINGREF ProgramName
    )
{
    if (!TmpRulesList)
        return NULL;

    for (ULONG i = 0; i < TmpRulesList->Count; i++)
    {
        PTMP_RULE rule = TmpRulesList->Items[i];

        if (PhEqualStringRef(&rule->ProgramName->sr, ProgramName, TRUE))
            return rule;
    }

    return NULL;
}

/**
 * Finds the first rule matching a process: same image name (case-insensitive) and,
 * when the rule has a path filter, the Win32 image path contains the filter text.
 */
PTMP_RULE TmpFindRuleForProcess(
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _Inout_ PTMP_PROCESS_EXTENSION Extension
    )
{
    if (!TmpRulesList || PhIsNullOrEmptyString(ProcessItem->ProcessName))
        return NULL;

    for (ULONG i = 0; i < TmpRulesList->Count; i++)
    {
        PTMP_RULE rule = TmpRulesList->Items[i];

        if (PhIsNullOrEmptyString(rule->ProgramName))
            continue;
        if (!PhEqualStringRef(&rule->ProgramName->sr, &ProcessItem->ProcessName->sr, TRUE))
            continue;

        if (!PhIsNullOrEmptyString(rule->MatchPath))
        {
            if (!Extension->FileNameWin32 && ProcessItem->FileName)
                Extension->FileNameWin32 = PhGetFileName(ProcessItem->FileName);

            if (!Extension->FileNameWin32)
                continue;
            if (PhFindStringInStringRef(&Extension->FileNameWin32->sr, &rule->MatchPath->sr, TRUE) == SIZE_MAX)
                continue;
        }

        return rule;
    }

    return NULL;
}

/**
 * Call after any change to the rule list or a rule's configuration.
 * Bumps the version (processes re-resolve their rule), saves and refreshes the tab.
 */
VOID TmpRulesChanged(
    VOID
    )
{
    TmpRulesVersion++;
    TmpSaveRules();
    TmpRefreshRulesTab();
}

// Persistence

/**
 * Returns the rules file path: a tmp-rules.json beside the executable wins
 * (portable mode), otherwise %APPDATA%\SystemInformer\tmp-rules.json.
 */
PPH_STRING TmpGetRulesFileName(
    VOID
    )
{
    PPH_STRING fileName;

    if (fileName = PhGetApplicationDirectoryFileName(&TmpRulesFileNameSr, FALSE))
    {
        if (PhDoesFileExistWin32(PhGetString(fileName)))
            return fileName;

        PhDereferenceObject(fileName);
    }

    if (fileName = PhGetRoamingAppDataDirectory(&TmpRulesRoamingPathSr, FALSE))
        return fileName;

    return PhGetApplicationDirectoryFileName(&TmpRulesFileNameSr, FALSE);
}

static VOID TmpAddJsonString(
    _In_ PVOID Object,
    _In_ PCSTR Key,
    _In_opt_ PPH_STRING Value
    )
{
    PPH_BYTES utf8;

    utf8 = PhConvertUtf16ToUtf8(PhGetStringOrEmpty(Value));
    PhAddJsonObjectUtf8(Object, Key, utf8);
    PhDereferenceObject(utf8);
}

static PVOID TmpRuleToJson(
    _In_ PTMP_RULE Rule
    )
{
    PVOID object;
    PVOID watchdogs;

    object = PhCreateJsonObject();
    PhAddJsonObjectUInt64(object, "id", Rule->Id);
    PhAddJsonObjectBoolean(object, "enabled", Rule->Enabled);
    PhAddJsonObjectBoolean(object, "muted", Rule->Muted);
    TmpAddJsonString(object, "program", Rule->ProgramName);
    TmpAddJsonString(object, "path", Rule->MatchPath);
    PhAddJsonObjectUInt64(object, "startPriority", Rule->StartPriority);
    PhAddJsonObjectUInt64(object, "startAffinity", Rule->StartAffinity);
    PhAddJsonObjectUInt64(object, "startAffinityMask", Rule->StartAffinityMask);
    PhAddJsonObjectBoolean(object, "startEfficiency", Rule->StartEfficiencyMode);
    PhAddJsonObjectBoolean(object, "startEnd", Rule->StartEnd);
    TmpAddJsonString(object, "command", Rule->Command);

    watchdogs = PhCreateJsonArray();

    for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
    {
        PTMP_WATCHDOG watchdog = &Rule->Watchdogs[i];
        PVOID entry = PhCreateJsonObject();

        PhAddJsonObjectBoolean(entry, "enabled", watchdog->Enabled);
        PhAddJsonObjectUInt64(entry, "metric", watchdog->Metric);
        PhAddJsonObjectUInt64(entry, "threshold", watchdog->Threshold);
        PhAddJsonObjectUInt64(entry, "duration", watchdog->DurationSeconds);
        PhAddJsonObjectUInt64(entry, "action", watchdog->Action);
        PhAddJsonArrayObject(watchdogs, entry);
    }

    PhAddJsonObjectValue(object, "watchdogs", watchdogs);

    return object;
}

static PPH_STRING TmpGetJsonString(
    _In_ PVOID Object,
    _In_ PCSTR Key
    )
{
    PPH_STRING value;

    if (value = PhGetJsonValueAsString(Object, Key))
        return value;

    return PhReferenceEmptyString();
}

static PTMP_RULE TmpRuleFromJson(
    _In_ PVOID Object
    )
{
    PTMP_RULE rule;
    PVOID watchdogs;

    rule = TmpCreateRule();
    rule->Enabled = PhGetJsonObjectBool(Object, "enabled");
    rule->Muted = PhGetJsonObjectBool(Object, "muted"); // absent in older files = FALSE
    PhMoveReference(&rule->ProgramName, TmpGetJsonString(Object, "program"));
    PhMoveReference(&rule->MatchPath, TmpGetJsonString(Object, "path"));
    PhMoveReference(&rule->Command, TmpGetJsonString(Object, "command"));
    rule->StartPriority = PhGetJsonValueAsUlong(Object, "startPriority");
    rule->StartAffinity = (TMP_AFFINITY_PRESET)PhGetJsonValueAsUlong(Object, "startAffinity");
    rule->StartAffinityMask = (KAFFINITY)PhGetJsonValueAsUInt64(Object, "startAffinityMask");
    rule->StartEfficiencyMode = PhGetJsonObjectBool(Object, "startEfficiency");
    rule->StartEnd = PhGetJsonObjectBool(Object, "startEnd");

    if (rule->StartPriority > PROCESS_PRIORITY_CLASS_ABOVE_NORMAL)
        rule->StartPriority = 0;
    if (rule->StartAffinity >= TmpAffinityMaximum)
        rule->StartAffinity = TmpAffinityNone;

    if (watchdogs = PhGetJsonObject(Object, "watchdogs"))
    {
        ULONG count = PhGetJsonArrayLength(watchdogs);

        for (ULONG i = 0; i < count && i < TMP_MAX_WATCHDOGS; i++)
        {
            PVOID entry = PhGetJsonArrayIndexObject(watchdogs, i);
            PTMP_WATCHDOG watchdog = &rule->Watchdogs[i];

            if (!entry)
                continue;

            watchdog->Enabled = PhGetJsonObjectBool(entry, "enabled");
            watchdog->Metric = (TMP_METRIC)PhGetJsonValueAsUlong(entry, "metric");
            watchdog->Threshold = PhGetJsonValueAsUInt64(entry, "threshold");
            watchdog->DurationSeconds = PhGetJsonValueAsUlong(entry, "duration");
            watchdog->Action = (TMP_ACTION)PhGetJsonValueAsUlong(entry, "action");

            if (watchdog->Metric >= TmpMetricMaximum)
                watchdog->Metric = TmpMetricCpu;
            if (watchdog->Action >= TmpActionMaximum)
                watchdog->Action = TmpActionNotify;
        }
    }

    return rule;
}

NTSTATUS TmpSaveRulesToFile(
    _In_ PCPH_STRINGREF FileName
    )
{
    NTSTATUS status;
    PVOID root;
    PVOID rules;
    PH_STRINGREF directory;
    PH_STRINGREF baseName;

    root = PhCreateJsonObject();
    PhAddJsonObjectUInt64(root, "version", TMP_RULES_FILE_VERSION);
    rules = PhCreateJsonArray();

    if (TmpRulesList)
    {
        for (ULONG i = 0; i < TmpRulesList->Count; i++)
        {
            PhAddJsonArrayObject(rules, TmpRuleToJson(TmpRulesList->Items[i]));
        }
    }

    PhAddJsonObjectValue(root, "rules", rules);

    if (PhSplitStringRefAtLastChar(FileName, OBJ_NAME_PATH_SEPARATOR, &directory, &baseName))
    {
        PhCreateDirectoryFullPathWin32(&directory);
    }

    status = PhSaveJsonObjectToFile(FileName, root, PH_JSON_TO_STRING_PRETTY);

    PhFreeJsonObject(root);

    return status;
}

/**
 * Loads rules from a file. When Merge is set, rules whose program name already
 * exists replace the existing rule; otherwise the current list is cleared first.
 */
NTSTATUS TmpLoadRulesFromFile(
    _In_ PCPH_STRINGREF FileName,
    _In_ BOOLEAN Merge
    )
{
    NTSTATUS status;
    PVOID root;
    PVOID rules;

    status = PhLoadJsonObjectFromFile(&root, FileName);

    if (!NT_SUCCESS(status))
        return status;

    if (!TmpRulesList)
        TmpRulesList = PhCreateList(16);

    if (!Merge)
    {
        for (ULONG i = 0; i < TmpRulesList->Count; i++)
            TmpDeleteRule(TmpRulesList->Items[i]);

        PhClearList(TmpRulesList);
    }

    if (rules = PhGetJsonObject(root, "rules"))
    {
        ULONG count = PhGetJsonArrayLength(rules);

        for (ULONG i = 0; i < count; i++)
        {
            PVOID entry = PhGetJsonArrayIndexObject(rules, i);
            PTMP_RULE rule;
            PTMP_RULE existing;

            if (!entry)
                continue;

            rule = TmpRuleFromJson(entry);

            if (PhIsNullOrEmptyString(rule->ProgramName))
            {
                TmpDeleteRule(rule);
                continue;
            }

            if (Merge && (existing = TmpFindRuleByProgramName(&rule->ProgramName->sr)))
            {
                TmpCopyRule(existing, rule);
                TmpDeleteRule(rule);
                continue;
            }

            if (TmpRulesList->Count >= TMP_MAX_RULES)
            {
                TmpDeleteRule(rule);
                break;
            }

            PhAddItemList(TmpRulesList, rule);
        }
    }

    PhFreeJsonObject(root);

    return STATUS_SUCCESS;
}

VOID TmpLoadRules(
    VOID
    )
{
    PPH_STRING fileName;

    if (!TmpRulesList)
        TmpRulesList = PhCreateList(16);

    if (fileName = TmpGetRulesFileName())
    {
        if (PhDoesFileExistWin32(PhGetString(fileName)))
        {
            TmpLoadRulesFromFile(&fileName->sr, FALSE);
        }

        PhDereferenceObject(fileName);
    }

    TmpRulesVersion++;
}

VOID TmpSaveRules(
    VOID
    )
{
    PPH_STRING fileName;

    if (fileName = TmpGetRulesFileName())
    {
        TmpSaveRulesToFile(&fileName->sr);
        PhDereferenceObject(fileName);
    }
}

// Core masks (P/E cores on hybrid CPUs)

static VOID TmpInitializeCoreMasks(
    VOID
    )
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer;
    ULONG bufferLength;
    BYTE maximumClass = 0;

    if (TmpCoreMasksInitialized)
        return;

    TmpCoreMasksInitialized = TRUE;

    if (!NT_SUCCESS(PhGetSystemLogicalProcessorInformation(RelationProcessorCore, &buffer, &bufferLength)))
        return;

    // First pass: find the highest efficiency class (higher = more performant).
    for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = buffer;
        (ULONG_PTR)info < (ULONG_PTR)buffer + bufferLength;
        info = PTR_ADD_OFFSET(info, info->Size))
    {
        if (info->Relationship != RelationProcessorCore)
            continue;
        if (info->Processor.GroupCount >= 1 && info->Processor.GroupMask[0].Group == 0)
            TmpAllCoreMask |= info->Processor.GroupMask[0].Mask;
        if (info->Processor.EfficiencyClass > maximumClass)
            maximumClass = info->Processor.EfficiencyClass;
    }

    if (maximumClass == 0)
    {
        PhFree(buffer); // homogeneous CPU
        return;
    }

    for (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = buffer;
        (ULONG_PTR)info < (ULONG_PTR)buffer + bufferLength;
        info = PTR_ADD_OFFSET(info, info->Size))
    {
        if (info->Relationship != RelationProcessorCore)
            continue;
        if (info->Processor.GroupCount < 1 || info->Processor.GroupMask[0].Group != 0)
            continue; // only processor group 0 is supported

        if (info->Processor.EfficiencyClass == maximumClass)
            TmpPCoreMask |= info->Processor.GroupMask[0].Mask;
        else
            TmpECoreMask |= info->Processor.GroupMask[0].Mask;
    }

    PhFree(buffer);
}

BOOLEAN TmpIsHybridSystem(
    VOID
    )
{
    TmpInitializeCoreMasks();
    return TmpPCoreMask != 0 && TmpECoreMask != 0;
}

KAFFINITY TmpGetCoreMask(
    _In_ BOOLEAN EfficientCores
    )
{
    TmpInitializeCoreMasks();
    return EfficientCores ? TmpECoreMask : TmpPCoreMask;
}

/**
 * Returns the affinity mask a rule's preset resolves to, or 0 when it does not apply.
 */
KAFFINITY TmpGetAffinityPresetMask(
    _In_ PTMP_RULE Rule
    )
{
    switch (Rule->StartAffinity)
    {
    case TmpAffinityAll:
        TmpInitializeCoreMasks();
        return TmpAllCoreMask;
    case TmpAffinityPCores:
        return TmpGetCoreMask(FALSE);
    case TmpAffinityECores:
        return TmpGetCoreMask(TRUE);
    case TmpAffinityCustom:
        return Rule->StartAffinityMask;
    }

    return 0;
}

// Display helpers

PCWSTR TmpGetMetricName(
    _In_ TMP_METRIC Metric
    )
{
    switch (Metric)
    {
    case TmpMetricCpu: return L"CPU";
    case TmpMetricPrivateBytes: return L"Private bytes";
    case TmpMetricWorkingSet: return L"Working set";
    case TmpMetricIoRate: return L"I/O rate";
    case TmpMetricHandles: return L"Handles";
    case TmpMetricThreads: return L"Threads";
    }

    return L"";
}

PCWSTR TmpGetMetricUnit(
    _In_ TMP_METRIC Metric
    )
{
    switch (Metric)
    {
    case TmpMetricCpu: return L"%";
    case TmpMetricPrivateBytes:
    case TmpMetricWorkingSet: return L"MB";
    case TmpMetricIoRate: return L"MB/s";
    }

    return L"";
}

PCWSTR TmpGetActionName(
    _In_ TMP_ACTION Action
    )
{
    switch (Action)
    {
    case TmpActionNotify: return L"Notify";
    case TmpActionLog: return L"Log only";
    case TmpActionLowerPriority: return L"Lower priority";
    case TmpActionEfficiencyMode: return L"Efficiency mode";
    case TmpActionSuspend: return L"Suspend";
    case TmpActionEnd: return L"End process";
    case TmpActionDump: return L"Write dump";
    case TmpActionRunCommand: return L"Run command";
    }

    return L"";
}

PCWSTR TmpGetPriorityName(
    _In_ ULONG PriorityClass
    )
{
    switch (PriorityClass)
    {
    case PROCESS_PRIORITY_CLASS_IDLE: return L"Idle";
    case PROCESS_PRIORITY_CLASS_BELOW_NORMAL: return L"Below normal";
    case PROCESS_PRIORITY_CLASS_NORMAL: return L"Normal";
    case PROCESS_PRIORITY_CLASS_ABOVE_NORMAL: return L"Above normal";
    case PROCESS_PRIORITY_CLASS_HIGH: return L"High";
    case PROCESS_PRIORITY_CLASS_REALTIME: return L"Real time";
    }

    return L"No change";
}

PCWSTR TmpGetAffinityPresetName(
    _In_ TMP_AFFINITY_PRESET Preset
    )
{
    switch (Preset)
    {
    case TmpAffinityAll: return L"All processors";
    case TmpAffinityPCores: return L"P-cores only";
    case TmpAffinityECores: return L"E-cores only";
    case TmpAffinityCustom: return L"Custom";
    }

    return L"No change";
}

/**
 * Formats a watchdog as e.g. "CPU > 80% for 30s: End process".
 */
PPH_STRING TmpFormatWatchdog(
    _In_ PTMP_WATCHDOG Watchdog
    )
{
    PH_FORMAT format[8];
    ULONG count = 0;

    PhInitFormatS(&format[count++], TmpGetMetricName(Watchdog->Metric));
    PhInitFormatS(&format[count++], L" > ");

    switch (Watchdog->Metric)
    {
    case TmpMetricPrivateBytes:
    case TmpMetricWorkingSet:
    case TmpMetricIoRate:
        PhInitFormatI64U(&format[count++], Watchdog->Threshold / (1024 * 1024));
        break;
    default:
        PhInitFormatI64U(&format[count++], Watchdog->Threshold);
        break;
    }

    PhInitFormatS(&format[count++], TmpGetMetricUnit(Watchdog->Metric));
    PhInitFormatS(&format[count++], L" for ");
    PhInitFormatU(&format[count++], Watchdog->DurationSeconds);
    PhInitFormatS(&format[count++], L"s: ");
    PhInitFormatS(&format[count++], TmpGetActionName(Watchdog->Action));

    return PhFormat(format, count, 64);
}

/**
 * Formats a rule's on-start actions as e.g. "Priority: Below normal; Affinity: E-cores only; Efficiency mode".
 */
PPH_STRING TmpFormatOnStart(
    _In_ PTMP_RULE Rule
    )
{
    PH_STRING_BUILDER sb;

    PhInitializeStringBuilder(&sb, 64);

    if (Rule->StartEnd)
        PhAppendStringBuilder2(&sb, L"End (blocked); ");
    if (Rule->StartPriority)
    {
        PhAppendStringBuilder2(&sb, L"Priority: ");
        PhAppendStringBuilder2(&sb, TmpGetPriorityName(Rule->StartPriority));
        PhAppendStringBuilder2(&sb, L"; ");
    }
    if (Rule->StartAffinity != TmpAffinityNone)
    {
        PhAppendStringBuilder2(&sb, L"Affinity: ");
        PhAppendStringBuilder2(&sb, TmpGetAffinityPresetName(Rule->StartAffinity));
        PhAppendStringBuilder2(&sb, L"; ");
    }
    if (Rule->StartEfficiencyMode)
        PhAppendStringBuilder2(&sb, L"Efficiency mode; ");
    if (!PhIsNullOrEmptyString(Rule->Command))
        PhAppendStringBuilder2(&sb, L"Run command; ");

    if (PhEndsWithString2(sb.String, L"; ", FALSE))
        PhRemoveEndStringBuilder(&sb, 2);

    return PhFinalStringBuilderString(&sb);
}
