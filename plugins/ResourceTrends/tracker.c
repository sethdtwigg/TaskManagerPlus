/*
 * TaskManagerPlus ResourceTrends: sampling timer, per-process series and the leak detector glue.
 *
 * Called from GeneralCallbackProcessesUpdated on the main thread. Every sample interval it
 *  - reads system CPU / physical memory / commit,
 *  - appends one point per process (private bytes) to an in-memory series and asks LkEvaluate whether it grows,
 *  - stores the system figures plus the busiest processes (by CPU and by private bytes) through store.c.
 */

#include "resourcetrends.h"

// Exported by SystemInformer.exe (SystemInformer.def, DATA) but not declared in the plugin SDK headers.
__declspec(dllimport) extern FLOAT PhCpuKernelUsage;
__declspec(dllimport) extern FLOAT PhCpuUserUsage;

typedef struct _TRK_ENTRY
{
    ULONG ProcessId;
    LONGLONG CreateTime;
    PPH_STRING Name;
    LK_POINT *Points;
    ULONG Count;
    ULONG Capacity;
    BOOLEAN Seen;
    BOOLEAN Suspect;
    BOOLEAN Notified;
    LK_RESULT Result;
} TRK_ENTRY, *PTRK_ENTRY;

#define TRK_MAX_POINTS 4096

static PPH_LIST TrkEntries = NULL; // PTRK_ENTRY
static PPH_LIST TrkSuspects = NULL; // PTRS_SUSPECT, rebuilt after every sample
static LONGLONG TrkLastSampleTime = 0;

static ULONG TrsClampSetting(
    _In_ PCWSTR Name,
    _In_ ULONG Minimum,
    _In_ ULONG Maximum
    )
{
    ULONG value = PhGetIntegerSetting(Name);

    if (value < Minimum)
        return Minimum;
    if (value > Maximum)
        return Maximum;
    return value;
}

ULONG TrsGetSampleSeconds(
    VOID
    )
{
    return TrsClampSetting(SETTING_NAME_SAMPLE_SECONDS, 1, 3600);
}

ULONG TrsGetRetentionHours(
    VOID
    )
{
    return TrsClampSetting(SETTING_NAME_RETENTION_HOURS, 1, 2160);
}

ULONG TrsGetMaxFileMB(
    VOID
    )
{
    return TrsClampSetting(SETTING_NAME_MAX_FILE_MB, 1, 64);
}

VOID TrsGetLeakConfig(
    _Out_ LK_CONFIG *Config
    )
{
    Config->WindowSeconds = TrsClampSetting(SETTING_NAME_LEAK_WINDOW_SECONDS, 10, 86400);
    Config->MinGrowthKB = (ULONG64)TrsClampSetting(SETTING_NAME_LEAK_MIN_GROWTH_MB, 1, 1048576) * 1024;
    Config->MinGrowthPercent = TrsClampSetting(SETTING_NAME_LEAK_MIN_PERCENT, 0, 1000);
    Config->MinRisePercent = 80;
    Config->MinPoints = 6;
}

static PTRK_ENTRY TrkFind(
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    )
{
    ULONG i;

    for (i = 0; i < TrkEntries->Count; i++)
    {
        PTRK_ENTRY entry = TrkEntries->Items[i];

        if (entry->ProcessId == ProcessId && entry->CreateTime == CreateTime)
            return entry;
    }

    return NULL;
}

static PTRK_ENTRY TrkCreate(
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime,
    _In_opt_ PPH_STRING Name
    )
{
    PTRK_ENTRY entry = PhAllocateZero(sizeof(TRK_ENTRY));

    entry->ProcessId = ProcessId;
    entry->CreateTime = CreateTime;

    if (Name)
        PhSetReference(&entry->Name, Name);
    else
        entry->Name = PhCreateString(L"");

    PhAddItemList(TrkEntries, entry);
    return entry;
}

static VOID TrkFree(
    _In_ PTRK_ENTRY Entry
    )
{
    PhClearReference(&Entry->Name);
    PhFree(Entry->Points);
    PhFree(Entry);
}

static VOID TrkAddPoint(
    _In_ PTRK_ENTRY Entry,
    _In_ LONGLONG Seconds,
    _In_ ULONG64 ValueKB,
    _In_ LONGLONG WindowSeconds
    )
{
    ULONG drop = 0;

    if (Entry->Count && Entry->Points[Entry->Count - 1].Time >= Seconds)
        return; // out of order or duplicate

    if (Entry->Count == Entry->Capacity)
    {
        Entry->Capacity = Entry->Capacity ? Entry->Capacity * 2 : 64;
        Entry->Points = PhReAllocate(Entry->Points, Entry->Capacity * sizeof(LK_POINT));
    }

    Entry->Points[Entry->Count].Time = Seconds;
    Entry->Points[Entry->Count].ValueKB = ValueKB;
    Entry->Count++;

    // keep a little more than the window so a changed setting still has data
    while (drop < Entry->Count && Entry->Points[drop].Time < Seconds - WindowSeconds - WindowSeconds / 4)
        drop++;

    if (Entry->Count - drop > TRK_MAX_POINTS)
        drop = Entry->Count - TRK_MAX_POINTS;

    if (drop)
    {
        memmove(Entry->Points, Entry->Points + drop, (Entry->Count - drop) * sizeof(LK_POINT));
        Entry->Count -= drop;
    }
}

VOID TrsTrackerSeed(
    _In_ const TR_SAMPLE *Sample,
    _In_ BOOLEAN IncludeSystem
    )
{
    LONGLONG seconds = Sample->System.Time / TR_FILETIME_SECOND;
    LK_CONFIG config;
    ULONG i;

    if (!TrkEntries)
        return;

    TrsGetLeakConfig(&config);

    if (IncludeSystem)
    {
        PTRK_ENTRY entry = TrkFind(0, 0);

        if (!entry)
            entry = TrkCreate(0, 0, NULL);

        TrkAddPoint(entry, seconds, (ULONG64)Sample->System.CommitUsedMB * 1024, config.WindowSeconds);
    }

    for (i = 0; i < Sample->Count; i++)
    {
        const TR_PROCESS *process = &Sample->Processes[i];
        PTRK_ENTRY entry = TrkFind(process->ProcessId, process->CreateTime);

        if (!entry)
            entry = TrkCreate(process->ProcessId, process->CreateTime, TrsStoreGetName(process->NameIndex));

        TrkAddPoint(entry, seconds, process->PrivateKB, config.WindowSeconds);
    }

    TrkLastSampleTime = Sample->System.Time;
}

ULONG TrsGetSuspectCount(
    VOID
    )
{
    return TrkSuspects ? TrkSuspects->Count : 0;
}

PTRS_SUSPECT TrsGetSuspect(
    _In_ ULONG Index
    )
{
    return TrkSuspects && Index < TrkSuspects->Count ? TrkSuspects->Items[Index] : NULL;
}

static VOID TrkClearSuspects(
    VOID
    )
{
    ULONG i;

    for (i = 0; i < TrkSuspects->Count; i++)
    {
        PTRS_SUSPECT suspect = TrkSuspects->Items[i];

        PhClearReference(&suspect->Name);
        PhFree(suspect);
    }

    PhClearList(TrkSuspects);
}

static int __cdecl TrkSuspectCompare(
    _In_ const void *A,
    _In_ const void *B
    )
{
    PTRS_SUSPECT a = *(PTRS_SUSPECT *)A;
    PTRS_SUSPECT b = *(PTRS_SUSPECT *)B;

    if (a->Result.GrowthKB != b->Result.GrowthKB)
        return a->Result.GrowthKB < b->Result.GrowthKB ? 1 : -1;

    return 0;
}

static VOID TrkNotify(
    _In_ PTRK_ENTRY Entry,
    _In_ ULONG WindowMinutes
    )
{
    PPH_STRING growth = PhFormatSize(Entry->Result.GrowthKB * 1024, ULONG_MAX);
    PPH_STRING rate = PhFormatSize((ULONG64)Entry->Result.RateKBPerMinute * 1024, ULONG_MAX);
    PPH_STRING text;

    if (Entry->ProcessId == 0)
    {
        text = PhFormatString(
            L"System commit charge grew by %s in %lu min (about %s per minute).",
            PhGetStringOrEmpty(growth),
            WindowMinutes,
            PhGetStringOrEmpty(rate)
            );
    }
    else
    {
        text = PhFormatString(
            L"%s (PID %lu) private bytes grew by %s in %lu min (about %s per minute).",
            PhGetStringOrEmpty(Entry->Name),
            Entry->ProcessId,
            PhGetStringOrEmpty(growth),
            WindowMinutes,
            PhGetStringOrEmpty(rate)
            );
    }

    PhLogMessageEntry(PH_LOG_ENTRY_MESSAGE, PhFormatString(L"Possible memory leak: %s", text->Buffer));

    if (PhGetIntegerSetting(SETTING_NAME_LEAK_NOTIFY))
        PhShowIconNotification(L"Possible memory leak", text->Buffer);

    PhDereferenceObject(text);
    PhDereferenceObject(rate);
    PhDereferenceObject(growth);
}

static VOID TrkEvaluate(
    VOID
    )
{
    LK_CONFIG config;
    ULONG i;

    TrkClearSuspects();
    TrsGetLeakConfig(&config);

    if (!PhGetIntegerSetting(SETTING_NAME_LEAK_ENABLED))
        return;

    for (i = 0; i < TrkEntries->Count; i++)
    {
        PTRK_ENTRY entry = TrkEntries->Items[i];
        LK_CONFIG entryConfig = config;
        PTRS_SUSPECT suspect;

        if (entry->ProcessId == 0)
            entryConfig.MinGrowthKB *= 3; // the system commit charge moves more than any process

        LkEvaluate(entry->Points, entry->Count, &entryConfig, &entry->Result);

        if (!entry->Result.Suspect)
        {
            entry->Suspect = FALSE;
            entry->Notified = FALSE;
            continue;
        }

        entry->Suspect = TRUE;

        if (!entry->Notified)
        {
            entry->Notified = TRUE;
            TrkNotify(entry, (ULONG)(config.WindowSeconds / 60));
        }

        suspect = PhAllocateZero(sizeof(TRS_SUSPECT));
        suspect->ProcessId = entry->ProcessId;
        suspect->CreateTime = entry->CreateTime;
        PhSetReference(&suspect->Name, entry->Name);
        suspect->CurrentKB = entry->Points[entry->Count - 1].ValueKB;
        suspect->Result = entry->Result;
        PhAddItemList(TrkSuspects, suspect);
    }

    if (TrkSuspects->Count > 1)
        qsort(TrkSuspects->Items, TrkSuspects->Count, sizeof(PVOID), TrkSuspectCompare);
}

typedef struct _TRK_CANDIDATE
{
    TR_PROCESS Record;
    PPH_STRING Name;
    BOOLEAN Selected;
} TRK_CANDIDATE;

static int __cdecl TrkCompareCpu(
    _In_ const void *A,
    _In_ const void *B
    )
{
    const TRK_CANDIDATE *a = A;
    const TRK_CANDIDATE *b = B;

    if (a->Record.CpuX100 != b->Record.CpuX100)
        return a->Record.CpuX100 < b->Record.CpuX100 ? 1 : -1;

    return 0;
}

static int __cdecl TrkCompareMemory(
    _In_ const void *A,
    _In_ const void *B
    )
{
    const TRK_CANDIDATE *a = A;
    const TRK_CANDIDATE *b = B;

    if (a->Record.PrivateKB != b->Record.PrivateKB)
        return a->Record.PrivateKB < b->Record.PrivateKB ? 1 : -1;

    return 0;
}

static VOID TrkWriteTestOutput(
    VOID
    )
{
    PPH_STRING path = PhGetStringSetting(SETTING_NAME_TEST_OUTPUT);
    PH_STRING_BUILDER sb;
    PPH_STRING text;
    PPH_BYTES utf8;
    TR_AGGREGATE *rows = NULL;
    ULONG inRange = 0;
    ULONG count;
    ULONG i;
    ULONG sampleCount = TrsStoreGetSampleCount();
    HANDLE fileHandle;
    LARGE_INTEGER position;
    ULONG written;

    if (PhIsNullOrEmptyString(path))
    {
        PhClearReference(&path);
        return;
    }

    PhInitializeStringBuilder(&sb, 256);
    PhAppendFormatStringBuilder(&sb, L"samples=%lu\r\nsuspects=%lu\r\n", sampleCount, TrsGetSuspectCount());

    for (i = 0; i < TrsGetSuspectCount(); i++)
    {
        PTRS_SUSPECT suspect = TrsGetSuspect(i);

        PhAppendFormatStringBuilder(
            &sb,
            L"suspect|%lu|%s|%I64u|%.0f\r\n",
            suspect->ProcessId,
            PhGetStringOrEmpty(suspect->Name),
            suspect->Result.GrowthKB,
            suspect->Result.RateKBPerMinute
            );
    }

    if (sampleCount)
    {
        TR_SAMPLE *const *samples = TrsStoreGetSamples();
        LONGLONG from = samples[0]->System.Time;
        LONGLONG to = samples[sampleCount - 1]->System.Time;

        count = TrAggregate(samples, sampleCount, from, to, 0, &rows, &inRange);

        for (i = 0; i < count && i < 5; i++)
        {
            PhAppendFormatStringBuilder(&sb, L"topcpu|%lu|%s|%.2f\r\n", rows[i].ProcessId, PhGetStringOrEmpty(TrsStoreGetName(rows[i].NameIndex)), rows[i].AvgCpuPercent);
        }

        TrFreeAggregate(rows);
        rows = NULL;
        count = TrAggregate(samples, sampleCount, from, to, 1, &rows, &inRange);

        for (i = 0; i < count && i < 5; i++)
        {
            PhAppendFormatStringBuilder(&sb, L"topmem|%lu|%s|%lu\r\n", rows[i].ProcessId, PhGetStringOrEmpty(TrsStoreGetName(rows[i].NameIndex)), rows[i].PeakPrivateKB);
        }

        TrFreeAggregate(rows);
    }

    text = PhFinalStringBuilderString(&sb);
    utf8 = PhConvertUtf16ToUtf8Ex(text->Buffer, text->Length);

    if (utf8 && NT_SUCCESS(PhCreateFileWin32(
        &fileHandle,
        path->Buffer,
        FILE_GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
        )))
    {
        position.QuadPart = 0;
        PhWriteFile(fileHandle, utf8->Buffer, (ULONG)utf8->Length, &position, &written);
        NtClose(fileHandle);
    }

    PhClearReference(&utf8);
    PhDereferenceObject(text);
    PhDereferenceObject(path);
}

VOID TrsCollect(
    VOID
    )
{
    LARGE_INTEGER now;
    PPH_PROCESS_ITEM *items;
    ULONG itemCount;
    ULONG i;
    ULONG candidateCount = 0;
    TRK_CANDIDATE *candidates;
    TR_SYSTEM system;
    TR_PROCESS *selected;
    ULONG selectedCount = 0;
    SYSTEM_PERFORMANCE_INFORMATION perf;
    LK_CONFIG config;
    LONGLONG seconds;
    ULONG pageKB;

    if (!TrkEntries)
    {
        TrkEntries = PhCreateList(512);
        TrkSuspects = PhCreateList(8);
    }

    PhQuerySystemTime(&now);

    if (TrkLastSampleTime && now.QuadPart - TrkLastSampleTime < (LONGLONG)TrsGetSampleSeconds() * TR_FILETIME_SECOND)
        return;

    TrkLastSampleTime = now.QuadPart;
    seconds = now.QuadPart / TR_FILETIME_SECOND;
    TrsGetLeakConfig(&config);

    memset(&system, 0, sizeof(system));
    system.Time = now.QuadPart;
    system.CpuX100 = (ULONG)((PhCpuKernelUsage + PhCpuUserUsage) * 10000.0f);
    system.KernelX100 = (ULONG)(PhCpuKernelUsage * 10000.0f);

    if (system.CpuX100 > 10000)
        system.CpuX100 = 10000;

    pageKB = PhSystemBasicInformation.PageSize / 1024;
    memset(&perf, 0, sizeof(perf));

    if (NT_SUCCESS(NtQuerySystemInformation(SystemPerformanceInformation, &perf, sizeof(perf), NULL)))
    {
        ULONG64 totalPages = PhSystemBasicInformation.NumberOfPhysicalPages;

        system.PhysicalTotalMB = (ULONG)(totalPages * pageKB / 1024);
        system.PhysicalUsedMB = (ULONG)((totalPages - min(totalPages, (ULONG64)perf.AvailablePages)) * pageKB / 1024);
        system.CommitUsedMB = (ULONG)((ULONG64)perf.CommittedPages * pageKB / 1024);
        system.CommitLimitMB = (ULONG)((ULONG64)perf.CommitLimit * pageKB / 1024);
    }

    {
        PTRK_ENTRY entry = TrkFind(0, 0);

        if (!entry)
            entry = TrkCreate(0, 0, NULL);

        entry->Seen = TRUE;
        PhMoveReference(&entry->Name, PhCreateString(L"System commit charge"));
        TrkAddPoint(entry, seconds, (ULONG64)system.CommitUsedMB * 1024, config.WindowSeconds);
    }

    PhEnumProcessItems(&items, &itemCount);
    candidates = PhAllocate(max(itemCount, 1) * sizeof(TRK_CANDIDATE));

    for (i = 0; i < TrkEntries->Count; i++)
    {
        PTRK_ENTRY entry = TrkEntries->Items[i];

        if (entry->ProcessId != 0)
            entry->Seen = FALSE;
    }

    for (i = 0; i < itemCount; i++)
    {
        PPH_PROCESS_ITEM item = items[i];
        TRK_CANDIDATE *candidate;
        PTRK_ENTRY entry;
        ULONG64 privateKB;
        ULONG processId;

        if (PH_IS_FAKE_PROCESS_ID(item->ProcessId) || item->ProcessId == SYSTEM_IDLE_PROCESS_ID)
            continue;

        processId = HandleToUlong(item->ProcessId);
        privateKB = (ULONG64)item->VmCounters.PagefileUsage / 1024;

        entry = TrkFind(processId, item->CreateTime.QuadPart);

        if (!entry)
            entry = TrkCreate(processId, item->CreateTime.QuadPart, item->ProcessName);

        entry->Seen = TRUE;
        TrkAddPoint(entry, seconds, privateKB, config.WindowSeconds);

        candidate = &candidates[candidateCount++];
        memset(candidate, 0, sizeof(TRK_CANDIDATE));
        candidate->Name = item->ProcessName; // not referenced: the items stay alive until the end of this function
        candidate->Record.ProcessId = processId;
        candidate->Record.CreateTime = item->CreateTime.QuadPart;
        candidate->Record.PrivateKB = (ULONG)min(privateKB, ULONG_MAX);
        candidate->Record.WorkingSetKB = (ULONG)min((ULONG64)item->VmCounters.WorkingSetSize / 1024, ULONG_MAX);
        candidate->Record.CpuX100 = (USHORT)min((ULONG)(item->CpuUsage * 10000.0f), 10000);
    }

    // Selection: the busiest by CPU and the largest by private bytes (flags travel with the sorted elements).
    qsort(candidates, candidateCount, sizeof(TRK_CANDIDATE), TrkCompareCpu);

    for (i = 0; i < candidateCount && i < TRS_TOP_BY_CPU && candidates[i].Record.CpuX100 != 0; i++)
        candidates[i].Selected = TRUE;

    qsort(candidates, candidateCount, sizeof(TRK_CANDIDATE), TrkCompareMemory);

    for (i = 0; i < candidateCount && i < TRS_TOP_BY_MEMORY; i++)
        candidates[i].Selected = TRUE;

    selected = PhAllocate(max(candidateCount, 1) * sizeof(TR_PROCESS));

    for (i = 0; i < candidateCount; i++)
    {
        if (candidates[i].Selected)
        {
            selected[selectedCount] = candidates[i].Record;
            selected[selectedCount].NameIndex = TrsStoreInternName(candidates[i].Name);
            selectedCount++;
        }
    }

    PhDereferenceObjects(items, itemCount);
    PhFree(items);
    PhFree(candidates);

    TrsStoreAddSample(&system, selected, selectedCount);
    PhFree(selected);

    // forget processes that are gone
    for (i = TrkEntries->Count; i > 0; i--)
    {
        PTRK_ENTRY entry = TrkEntries->Items[i - 1];

        if (!entry->Seen)
        {
            PhRemoveItemList(TrkEntries, i - 1);
            TrkFree(entry);
        }
    }

    TrkEvaluate();
    TrkWriteTestOutput();
    TrsTabDataChanged();
}

VOID TrsTrackerShutdown(
    VOID
    )
{
    ULONG i;

    if (!TrkEntries)
        return;

    TrkClearSuspects();

    for (i = 0; i < TrkEntries->Count; i++)
        TrkFree(TrkEntries->Items[i]);

    PhClearList(TrkEntries);
}
