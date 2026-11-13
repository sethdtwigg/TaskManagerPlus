/*
 * TaskManagerPlus EfficiencyAdvisor: foreground detection, per-process sampling and the ranked row list.
 */

#include "effadvisor.h"

static ULONG64 EaLastSampleTick = 0;
static ULONG EaLastExternalForeground = 0;
static ULONG EaWatched = 0;

VOID EaGetConfig(
    _Out_ AD_CONFIG *Config
    )
{
    AdDefaultConfig(Config);
    Config->MinBackgroundSeconds = (double)PhGetIntegerSetting(SETTING_NAME_MIN_BACKGROUND_SECONDS);
    Config->HalfLifeSeconds = (double)PhGetIntegerSetting(SETTING_NAME_HALF_LIFE_SECONDS);
    Config->CpuThresholdPercent = (double)PhGetIntegerSetting(SETTING_NAME_CPU_THRESHOLD_TENTHS) / 10.0;
    Config->MemoryThresholdMB = (double)PhGetIntegerSetting(SETTING_NAME_MEMORY_THRESHOLD_MB);
    Config->IoThresholdMBps = (double)PhGetIntegerSetting(SETTING_NAME_IO_THRESHOLD_KBPS) / 1024.0;
    Config->MinScore = (double)PhGetIntegerSetting(SETTING_NAME_MIN_SCORE_TENTHS) / 10.0;
}

VOID EaTrackerInitialize(
    VOID
    )
{
    EaLastSampleTick = 0;
}

ULONG EaGetWatchedCount(
    VOID
    )
{
    return EaWatched;
}

static PEA_EXTENSION EaGetExtension(
    _In_ PPH_PROCESS_ITEM Process
    )
{
    return PhPluginGetObjectExtension(PluginInstance, Process, EmProcessItemType);
}

static BOOLEAN EaSameFile(
    _In_opt_ PPH_STRING A,
    _In_opt_ PPH_STRING B
    )
{
    return A && B && A->Length && PhEqualString(A, B, TRUE);
}

/** TRUE when Process is the foreground process, a descendant of it, or runs the same image. */
static BOOLEAN EaIsForegroundApp(
    _In_ PPH_PROCESS_ITEM Process,
    _In_ ULONG ForegroundPid,
    _In_opt_ PPH_PROCESS_ITEM ForegroundItem
    )
{
    HANDLE parentId;
    PPH_PROCESS_ITEM child;
    ULONG depth;
    BOOLEAN result = FALSE;

    if (HandleToUlong(Process->ProcessId) == ForegroundPid)
        return TRUE;

    if (ForegroundItem && EaSameFile(Process->FileName, ForegroundItem->FileName))
        return TRUE;

    // descendant of the foreground process (browsers, editors and games start helper processes)
    child = Process;
    parentId = Process->ParentProcessId;
    PhReferenceObject(child);

    for (depth = 0; depth < 16 && parentId && HandleToUlong(parentId) != 0; depth++)
    {
        PPH_PROCESS_ITEM parent;

        if (HandleToUlong(parentId) == ForegroundPid)
        {
            PPH_PROCESS_ITEM fg = PhReferenceProcessItem(parentId);

            // a reused pid is not a parent: the parent must be older than the child
            result = fg && fg->CreateTime.QuadPart <= child->CreateTime.QuadPart;

            if (fg)
                PhDereferenceObject(fg);

            break;
        }

        parent = PhReferenceProcessItem(parentId);

        if (!parent || parent->CreateTime.QuadPart > child->CreateTime.QuadPart)
        {
            if (parent)
                PhDereferenceObject(parent);

            break;
        }

        PhDereferenceObject(child);
        child = parent;
        parentId = child->ParentProcessId;
    }

    PhDereferenceObject(child);
    return result;
}

static BOOLEAN EaIsExcludedProcess(
    _In_ PPH_PROCESS_ITEM Process,
    _In_ PCWSTR IgnoreList
    )
{
    PCWSTR name = PhGetStringOrEmpty(Process->ProcessName);
    PPH_STRING user = Process->UserName;

    if (PH_IS_FAKE_PROCESS_ID(Process->ProcessId) || HandleToUlong(Process->ProcessId) <= 4)
        return TRUE;

    if (Process->ProcessId == NtCurrentProcessId() || Process->IsProtectedProcess || Process->IsSecureProcess || Process->IsSystemProcess)
        return TRUE;

    if (Process->SessionId == 0)
        return TRUE; // services session

    if (user && (PhStartsWithString2(user, L"NT AUTHORITY\\", TRUE) || PhStartsWithString2(user, L"NT SERVICE\\", TRUE) ||
        PhStartsWithString2(user, L"Window Manager\\", TRUE) || PhStartsWithString2(user, L"Font Driver Host\\", TRUE)))
        return TRUE;

    if (AdIsCriticalName(name))
        return TRUE;

    return AdIsIgnored(IgnoreList, name) ? TRUE : FALSE;
}

VOID EaSample(
    VOID
    )
{
    ULONG64 now = NtGetTickCount64();
    double seconds;
    HWND foreground;
    ULONG foregroundPid = 0;
    PPH_PROCESS_ITEM foregroundItem = NULL;
    PPH_PROCESS_ITEM *items;
    ULONG count;
    ULONG i;
    AD_CONFIG config;
    PPH_STRING ignored;
    ULONG watched = 0;

    if (!EaLastSampleTick)
    {
        EaLastSampleTick = now;
        return;
    }

    seconds = (double)(now - EaLastSampleTick) / 1000.0;
    EaLastSampleTick = now;

    if (seconds <= 0.0 || seconds > 120.0)
        return; // first sample after a sleep or a very long stall

    foreground = GetForegroundWindow();

    if (!foreground && !PhGetIntegerSetting(SETTING_NAME_TEST_ALLOW_NO_FOREGROUND))
        return; // locked screen or secure desktop: learn nothing

    if (foreground)
        GetWindowThreadProcessId(foreground, &foregroundPid);

    if (foregroundPid == HandleToUlong(NtCurrentProcessId()))
        foregroundPid = EaLastExternalForeground; // looking at this window: the app used before it is still the foreground one
    else if (foregroundPid)
        EaLastExternalForeground = foregroundPid;

    EaGetConfig(&config);
    ignored = PhGetStringSetting(SETTING_NAME_IGNORED);

    if (foregroundPid)
        foregroundItem = PhReferenceProcessItem(UlongToHandle(foregroundPid));

    PhEnumProcessItems(&items, &count);

    for (i = 0; i < count; i++)
    {
        PPH_PROCESS_ITEM p = items[i];
        PEA_EXTENSION ext = EaGetExtension(p);

        if (ext)
        {
            double cpu = (double)p->CpuUsage * 100.0;
            double io = ((double)p->IoReadDelta.Delta + (double)p->IoWriteDelta.Delta) / seconds;
            BOOLEAN background;

            ext->Excluded = EaIsExcludedProcess(p, ignored->Buffer);

            if (!ext->Excluded)
            {
                background = !EaIsForegroundApp(p, foregroundPid, foregroundItem);
                AdUpdate(&ext->Stats, &config, seconds, background, cpu, io, p->VmCounters.WorkingSetSize / 1024, p->VmCounters.PagefileUsage / 1024);

                if (background)
                    watched++;
            }
        }

        PhDereferenceObject(p);
    }

    PhFree(items);

    if (foregroundItem)
        PhDereferenceObject(foregroundItem);

    PhDereferenceObject(ignored);
    EaWatched = watched;
}

static PPH_STRING EaFormatBytes(
    _In_ ULONG64 Kilobytes
    )
{
    if (Kilobytes >= 1024 * 1024)
        return PhFormatString(L"%.1f GB", (double)Kilobytes / (1024.0 * 1024.0));

    return PhFormatString(L"%.0f MB", (double)Kilobytes / 1024.0);
}

static PPH_STRING EaFormatDuration(
    _In_ double Seconds
    )
{
    ULONG s = (ULONG)Seconds;

    if (s >= 3600)
        return PhFormatString(L"%lu h %lu min", s / 3600, (s % 3600) / 60);

    if (s >= 60)
        return PhFormatString(L"%lu min", s / 60);

    return PhFormatString(L"%lu s", s);
}

static int __cdecl EaScoreCompare(
    _In_ const void *A,
    _In_ const void *B
    )
{
    PEA_ROW a = *(PEA_ROW *)A;
    PEA_ROW b = *(PEA_ROW *)B;

    if (a->Score != b->Score)
        return a->Score < b->Score ? 1 : -1;

    return a->ProcessId < b->ProcessId ? -1 : a->ProcessId > b->ProcessId ? 1 : 0;
}

static VOID EaWriteTestOutput(
    _In_ PPH_LIST Rows
    )
{
    PPH_STRING path = PhGetStringSetting(SETTING_NAME_TEST_OUTPUT);
    PH_STRING_BUILDER sb;
    ULONG i;

    if (path->Length)
    {
        HANDLE fileHandle;
        PPH_STRING text;
        PPH_BYTES utf8;

        PhInitializeStringBuilder(&sb, 256);

        for (i = 0; i < Rows->Count; i++)
        {
            PEA_ROW row = Rows->Items[i];

            text = PhFormatString(L"%s|%lu|%.2f|%s|%s|%s|%s\r\n", row->Name->Buffer, row->ProcessId, row->Score, row->Why->Buffer, row->Cpu->Buffer, row->Memory->Buffer, row->Disk->Buffer);
            PhAppendStringBuilder(&sb, &text->sr);
            PhDereferenceObject(text);
        }

        text = PhFinalStringBuilderString(&sb);
        utf8 = PhConvertUtf16ToUtf8Ex(text->Buffer, text->Length);

        if (NT_SUCCESS(PhCreateFileWin32(&fileHandle, path->Buffer, FILE_GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE)))
        {
            PhWriteFile(fileHandle, utf8->Buffer, (ULONG)utf8->Length, NULL, NULL);
            NtClose(fileHandle);
        }

        PhDereferenceObject(utf8);
        PhDereferenceObject(text);
    }

    PhDereferenceObject(path);
}

VOID EaFreeRows(
    _In_ PPH_LIST Rows
    )
{
    ULONG i;

    for (i = 0; i < Rows->Count; i++)
    {
        PEA_ROW row = Rows->Items[i];

        PhClearReference(&row->Name);
        PhClearReference(&row->Why);
        PhClearReference(&row->Cpu);
        PhClearReference(&row->Memory);
        PhClearReference(&row->Disk);
        PhClearReference(&row->Background);
        PhClearReference(&row->Efficiency);
        PhClearReference(&row->Priority);
        PhClearReference(&row->ScoreText);
        PhClearReference(&row->PidText);
        PhFree(row);
    }

    PhDereferenceObject(Rows);
}

PPH_LIST EaBuildRows(
    VOID
    )
{
    PPH_PROCESS_ITEM *items;
    ULONG count;
    ULONG i;
    AD_CONFIG config;
    PPH_LIST rows = PhCreateList(32);
    AD_CANDIDATE *candidates;
    PPH_PROCESS_ITEM *itemOf;
    uint32_t *order;
    uint32_t ranked;

    EaGetConfig(&config);
    PhEnumProcessItems(&items, &count);
    candidates = PhAllocateZero(sizeof(AD_CANDIDATE) * (count + 1));
    itemOf = PhAllocateZero(sizeof(PVOID) * (count + 1));
    order = PhAllocateZero(sizeof(uint32_t) * (count + 1));

    for (i = 0; i < count; i++)
    {
        PEA_EXTENSION ext = EaGetExtension(items[i]);

        itemOf[i] = items[i];
        candidates[i].ProcessId = HandleToUlong(items[i]->ProcessId);
        candidates[i].Excluded = !ext || ext->Excluded;

        if (ext)
            candidates[i].Stats = ext->Stats;
    }

    ranked = AdRank(candidates, count, &config, order);

    for (i = 0; i < ranked; i++)
    {
        AD_CANDIDATE *c = &candidates[order[i]];
        PPH_PROCESS_ITEM p = itemOf[order[i]];
        PEA_ROW row = PhAllocateZero(sizeof(EA_ROW));
        PCPH_STRINGREF priority = p->PriorityClass ? PhGetProcessPriorityClassString(p->PriorityClass) : NULL;
        PPH_STRING mem;
        PPH_STRING time;

        PhInitializeTreeNewNode(&row->Node);
        row->ProcessId = c->ProcessId;
        row->CreateTime = p->CreateTime.QuadPart;
        PhSetReference(&row->Name, p->ProcessName);
        row->Score = c->Score;
        row->Reason = c->Reason;
        row->Stats = c->Stats;
        row->EfficiencyMode = !!p->IsPowerThrottling;
        row->PriorityClass = p->PriorityClass;

        mem = EaFormatBytes(c->Stats.WorkingSetKB);
        time = EaFormatDuration(c->Stats.BackgroundSeconds);
        row->PidText = PhFormatString(L"%lu", row->ProcessId);
        row->Cpu = PhFormatString(L"%.1f %%", c->Stats.AvgCpuPercent);
        row->Memory = mem;
        row->Disk = PhFormatString(L"%.1f MB/s", c->Stats.AvgIoBytesPerSecond / (1024.0 * 1024.0));
        row->Background = time;
        row->Efficiency = PhCreateString(p->IsPowerThrottling ? L"On" : L"Off");
        row->Priority = priority ? PhCreateString2(priority) : PhReferenceEmptyString();
        row->ScoreText = PhFormatString(L"%.1f", c->Score);
        row->Why = PhFormatString(
            L"%s: %.1f %% CPU, %s, %.1f MB/s disk while in the background for %s",
            c->Reason == AdReasonCpu ? L"CPU" : c->Reason == AdReasonDisk ? L"Disk" : L"Memory",
            c->Stats.AvgCpuPercent, mem->Buffer, c->Stats.AvgIoBytesPerSecond / (1024.0 * 1024.0), time->Buffer
            );
        PhAddItemList(rows, row);
    }

    for (i = 0; i < count; i++)
        PhDereferenceObject(items[i]);

    PhFree(items);
    PhFree(candidates);
    PhFree(itemOf);
    PhFree(order);
    qsort(rows->Items, rows->Count, sizeof(PVOID), EaScoreCompare);
    EaWriteTestOutput(rows);
    return rows;
}

VOID EaAddIgnored(
    _In_ PPH_STRING Name
    )
{
    PPH_STRING list = PhGetStringSetting(SETTING_NAME_IGNORED);
    PPH_STRING updated;

    if (!AdIsIgnored(list->Buffer, Name->Buffer))
    {
        updated = PhFormatString(L"%s%s\n", list->Buffer, Name->Buffer);
        PhSetStringSetting2(SETTING_NAME_IGNORED, &updated->sr);
        PhDereferenceObject(updated);
    }

    PhDereferenceObject(list);
}

VOID NTAPI EaProcessItemCreateCallback(
    _In_ PVOID Object,
    _In_ PH_EM_OBJECT_TYPE ObjectType,
    _In_ PVOID Extension
    )
{
    PEA_EXTENSION ext = Extension;

    ext->Excluded = TRUE; // until the first sample says otherwise
}
