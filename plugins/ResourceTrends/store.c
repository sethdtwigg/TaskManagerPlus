/*
 * TaskManagerPlus ResourceTrends: sample store, trend file persistence, thinning and pruning.
 *
 * Everything runs on the main thread (the collector is called from GeneralCallbackProcessesUpdated), so there is
 * no locking. File format: see trendfmt.h and docs\resource-trends.md.
 */

#include "resourcetrends.h"

#define TRS_MAX_FILE_BYTES (64ULL * 1024 * 1024)
#define TRS_READ_CHUNK (1024 * 1024)

static PPH_LIST TrsSamples = NULL; // TR_SAMPLE*, ascending by time
static PPH_LIST TrsNames = NULL; // PPH_STRING, index = name index
static PPH_STRING TrsFileName = NULL;
static HANDLE TrsFileHandle = NULL;
static LARGE_INTEGER TrsFilePosition;
static ULONG TrsStringsWritten = 0;
static ULONG64 TrsStaleBytes = 0;
static BOOLEAN TrsPersist = FALSE;

static VOID TrsCloseFile(
    VOID
    )
{
    if (TrsFileHandle)
    {
        NtClose(TrsFileHandle);
        TrsFileHandle = NULL;
    }
}

static BOOLEAN TrsWriteBytes(
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    NTSTATUS status;
    ULONG written = 0;

    if (!TrsFileHandle)
        return FALSE;

    status = PhWriteFile(TrsFileHandle, Buffer, Length, &TrsFilePosition, &written);

    if (!NT_SUCCESS(status) || written != Length)
    {
        TrsCloseFile(); // give up on persistence for this session instead of corrupting the file
        return FALSE;
    }

    TrsFilePosition.QuadPart += Length;
    return TRUE;
}

static BOOLEAN TrsOpenFile(
    _In_ BOOLEAN Overwrite
    )
{
    NTSTATUS status;

    TrsCloseFile();

    if (!TrsFileName || !TrsPersist)
        return FALSE;

    PhCreateDirectoryFullPathWin32(&TrsFileName->sr);

    status = PhCreateFileWin32(
        &TrsFileHandle,
        TrsFileName->Buffer,
        FILE_GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        Overwrite ? FILE_OVERWRITE_IF : FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
        );

    if (!NT_SUCCESS(status))
    {
        TrsFileHandle = NULL;
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN TrsWriteString(
    _In_ PPH_STRING String
    )
{
    ULONG chars = (ULONG)(String->Length / sizeof(WCHAR));
    ULONG size;
    PUCHAR buffer;
    BOOLEAN ok;

    if (chars > 4096)
        chars = 4096;

    size = TR_RECORD_HEAD_BYTES + chars * 2;
    buffer = PhAllocate(size);
    ok = TrEncodeString(buffer, size, (const uint16_t *)String->Buffer, chars) == size && TrsWriteBytes(buffer, size);
    PhFree(buffer);
    return ok;
}

static BOOLEAN TrsWriteSample(
    _In_ const TR_SAMPLE *Sample
    )
{
    ULONG size = TrEncodeSampleSize(Sample->Count);
    PUCHAR buffer = PhAllocate(size);
    BOOLEAN ok;

    ok = TrEncodeSample(buffer, size, &Sample->System, Sample->Processes, Sample->Count) == size && TrsWriteBytes(buffer, size);
    PhFree(buffer);
    return ok;
}

static VOID TrsFreeSample(
    _In_ TR_SAMPLE *Sample
    )
{
    free(Sample->Processes);
    free(Sample);
}

/** Rewrites the file from memory: only used names (remapped), only kept samples. */
static VOID TrsRewriteFile(
    VOID
    )
{
    ULONG nameCount = TrsNames->Count;
    PULONG remap;
    PUCHAR used;
    PPH_LIST newNames;
    ULONG i;
    ULONG j;
    UCHAR header[TR_HEADER_BYTES];

    if (!TrsPersist || !TrsFileName)
        return;

    remap = PhAllocateZero(max(nameCount, 1) * sizeof(ULONG));
    used = PhAllocateZero(max(nameCount, 1));

    for (i = 0; i < TrsSamples->Count; i++)
    {
        TR_SAMPLE *sample = TrsSamples->Items[i];

        for (j = 0; j < sample->Count; j++)
        {
            if (sample->Processes[j].NameIndex < nameCount)
                used[sample->Processes[j].NameIndex] = 1;
        }
    }

    newNames = PhCreateList(256);

    for (i = 0; i < nameCount; i++)
    {
        if (used[i])
        {
            remap[i] = newNames->Count;
            PhAddItemList(newNames, TrsNames->Items[i]);
        }
        else
        {
            PhDereferenceObject(TrsNames->Items[i]);
        }
    }

    for (i = 0; i < TrsSamples->Count; i++)
    {
        TR_SAMPLE *sample = TrsSamples->Items[i];

        for (j = 0; j < sample->Count; j++)
        {
            if (sample->Processes[j].NameIndex < nameCount)
                sample->Processes[j].NameIndex = remap[sample->Processes[j].NameIndex];
        }
    }

    PhDereferenceObject(TrsNames);
    TrsNames = newNames;
    PhFree(remap);
    PhFree(used);

    if (!TrsOpenFile(TRUE))
        return;

    TrsFilePosition.QuadPart = 0;
    TrsStaleBytes = 0;
    TrsStringsWritten = 0;

    if (TrEncodeHeader(header, sizeof(header)) != sizeof(header) || !TrsWriteBytes(header, sizeof(header)))
        return;

    for (i = 0; i < TrsNames->Count; i++)
    {
        if (!TrsWriteString(TrsNames->Items[i]))
            return;

        TrsStringsWritten++;
    }

    for (i = 0; i < TrsSamples->Count; i++)
    {
        if (!TrsWriteSample(TrsSamples->Items[i]))
            return;
    }
}

static VOID TrsRemoveSamples(
    _In_ PUCHAR Keep
    )
{
    ULONG i;
    ULONG out = 0;

    for (i = 0; i < TrsSamples->Count; i++)
    {
        TR_SAMPLE *sample = TrsSamples->Items[i];

        if (Keep[i])
        {
            TrsSamples->Items[out++] = sample;
        }
        else
        {
            TrsStaleBytes += TrEncodeSampleSize(sample->Count);
            TrsFreeSample(sample);
        }
    }

    TrsSamples->Count = out;
}

static VOID TrsThinAndPrune(
    _In_ LONGLONG Now
    )
{
    ULONG count = TrsSamples->Count;
    PLONGLONG times;
    PUCHAR keep;
    ULONG i;
    ULONG kept;
    LONGLONG cutoff = Now - (LONGLONG)TrsGetRetentionHours() * TR_FILETIME_HOUR;
    ULONG64 estimate = 0;
    ULONG64 cap = (ULONG64)TrsGetMaxFileMB() * 1024 * 1024;

    if (!count)
        return;

    times = PhAllocate(count * sizeof(LONGLONG));
    keep = PhAllocate(count);

    for (i = 0; i < count; i++)
        times[i] = ((TR_SAMPLE *)TrsSamples->Items[i])->System.Time;

    kept = TrThin(times, count, Now, keep);

    // retention
    for (i = 0; i < count; i++)
    {
        if (times[i] < cutoff && keep[i])
        {
            keep[i] = 0;
            kept--;
        }
    }

    // size cap: drop the oldest kept samples until the estimated live size fits
    for (i = 0; i < count; i++)
    {
        if (keep[i])
            estimate += TrEncodeSampleSize(((TR_SAMPLE *)TrsSamples->Items[i])->Count);
    }

    for (i = 0; i < count && estimate > cap; i++)
    {
        if (keep[i])
        {
            keep[i] = 0;
            kept--;
            estimate -= TrEncodeSampleSize(((TR_SAMPLE *)TrsSamples->Items[i])->Count);
        }
    }

    if (kept != count)
        TrsRemoveSamples(keep);

    PhFree(times);
    PhFree(keep);
}

static VOID TrsMaybeCompact(
    VOID
    )
{
    ULONG64 total = (ULONG64)TrsFilePosition.QuadPart;

    if (!TrsPersist)
        return;

    if (TrsStaleBytes > 2 * 1024 * 1024 || (total > 256 * 1024 && TrsStaleBytes * 4 > total))
        TrsRewriteFile();
}

ULONG TrsStoreInternName(
    _In_ PPH_STRING Name
    )
{
    ULONG i;
    PH_STRINGREF empty = PH_STRINGREF_INIT(L"");
    PPH_STRING copy;

    if (!Name)
        Name = PhCreateString2(&empty);
    else
        PhReferenceObject(Name);

    // newest names are most likely to match: search from the end
    for (i = TrsNames->Count; i > 0; i--)
    {
        PPH_STRING existing = TrsNames->Items[i - 1];

        if (existing->Length == Name->Length && memcmp(existing->Buffer, Name->Buffer, Name->Length) == 0)
        {
            PhDereferenceObject(Name);
            return i - 1;
        }
    }

    copy = Name;
    PhAddItemList(TrsNames, copy);
    return TrsNames->Count - 1;
}

PPH_STRING TrsStoreGetName(
    _In_ ULONG Index
    )
{
    if (TrsNames && Index < TrsNames->Count)
        return TrsNames->Items[Index];

    return NULL;
}

VOID TrsStoreAddSample(
    _In_ const TR_SYSTEM *System,
    _In_reads_(Count) const TR_PROCESS *Processes,
    _In_ ULONG Count
    )
{
    TR_SAMPLE *sample = calloc(1, sizeof(TR_SAMPLE));

    if (!sample)
        return;

    sample->System = *System;
    sample->Count = Count;

    if (Count)
    {
        sample->Processes = malloc(Count * sizeof(TR_PROCESS));

        if (!sample->Processes)
        {
            free(sample);
            return;
        }

        memcpy(sample->Processes, Processes, Count * sizeof(TR_PROCESS));
    }

    PhAddItemList(TrsSamples, sample);

    if (TrsPersist && TrsFileHandle)
    {
        while (TrsStringsWritten < TrsNames->Count)
        {
            if (!TrsWriteString(TrsNames->Items[TrsStringsWritten]))
                break;

            TrsStringsWritten++;
        }

        if (TrsFileHandle)
            TrsWriteSample(sample);
    }

    TrsThinAndPrune(System->Time);
    TrsMaybeCompact();
}

ULONG TrsStoreGetSampleCount(
    VOID
    )
{
    return TrsSamples ? TrsSamples->Count : 0;
}

TR_SAMPLE *TrsStoreGetSample(
    _In_ ULONG Index
    )
{
    return Index < TrsStoreGetSampleCount() ? TrsSamples->Items[Index] : NULL;
}

TR_SAMPLE *const *TrsStoreGetSamples(
    VOID
    )
{
    return TrsSamples ? (TR_SAMPLE *const *)TrsSamples->Items : NULL;
}

ULONG64 TrsStoreGetFileBytes(
    VOID
    )
{
    return TrsFileHandle ? (ULONG64)TrsFilePosition.QuadPart : 0;
}

VOID TrsStoreInitialize(
    VOID
    )
{
    TrsSamples = PhCreateList(1024);
    TrsNames = PhCreateList(256);
}

static int TrsOnString(
    _In_ void *Context,
    _In_ const uint8_t *Text,
    _In_ uint32_t Chars
    )
{
    PWCHAR copy = PhAllocate((SIZE_T)Chars * sizeof(WCHAR) + sizeof(WCHAR));

    memcpy(copy, Text, (SIZE_T)Chars * sizeof(WCHAR));
    copy[Chars] = UNICODE_NULL;
    PhAddItemList(TrsNames, PhCreateStringEx(copy, (SIZE_T)Chars * sizeof(WCHAR)));
    PhFree(copy);
    return 1;
}

static int TrsOnSample(
    _In_ void *Context,
    _In_ const TR_SYSTEM *System,
    _In_ const TR_PROCESS *Processes,
    _In_ uint32_t Count
    )
{
    TR_SAMPLE *sample = calloc(1, sizeof(TR_SAMPLE));
    ULONG i;

    if (!sample)
        return 0;

    sample->System = *System;
    sample->Count = Count;

    if (Count)
    {
        sample->Processes = malloc(Count * sizeof(TR_PROCESS));

        if (!sample->Processes)
        {
            free(sample);
            return 0;
        }

        memcpy(sample->Processes, Processes, Count * sizeof(TR_PROCESS));

        // names must exist: a malformed index would crash the tab, so clamp it
        for (i = 0; i < Count; i++)
        {
            if (sample->Processes[i].NameIndex >= TrsNames->Count)
                sample->Processes[i].NameIndex = 0;
        }
    }

    // keep ascending order even if the file was written across a clock change
    if (TrsSamples->Count && ((TR_SAMPLE *)TrsSamples->Items[TrsSamples->Count - 1])->System.Time > System->Time)
    {
        TrsFreeSample(sample);
        return 1;
    }

    PhAddItemList(TrsSamples, sample);
    return 1;
}

static PVOID TrsReadFile(
    _Out_ PSIZE_T Size
    )
{
    NTSTATUS status;
    HANDLE fileHandle;
    LARGE_INTEGER fileSize;
    PVOID buffer = NULL;
    SIZE_T offset = 0;

    status = PhCreateFileWin32(
        &fileHandle,
        TrsFileName->Buffer,
        FILE_GENERIC_READ,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
        );

    if (!NT_SUCCESS(status))
        return NULL;

    if (NT_SUCCESS(PhGetFileSize(fileHandle, &fileSize)) &&
        fileSize.QuadPart > 0 &&
        (ULONG64)fileSize.QuadPart <= TRS_MAX_FILE_BYTES)
    {
        if (buffer = PhAllocateSafe((SIZE_T)fileSize.QuadPart))
        {
            while (offset < (SIZE_T)fileSize.QuadPart)
            {
                LARGE_INTEGER position;
                ULONG toRead;
                ULONG read = 0;

                position.QuadPart = offset;
                toRead = (ULONG)min((SIZE_T)fileSize.QuadPart - offset, TRS_READ_CHUNK);
                status = PhReadFile(fileHandle, PTR_ADD_OFFSET(buffer, offset), toRead, &position, &read);

                if (!NT_SUCCESS(status) || read == 0)
                    break;

                offset += read;
            }

            if (offset != (SIZE_T)fileSize.QuadPart)
            {
                PhFree(buffer);
                buffer = NULL;
            }
        }
    }

    NtClose(fileHandle);

    if (buffer)
        *Size = offset;

    return buffer;
}

VOID TrsStoreLoad(
    VOID
    )
{
    PVOID buffer;
    SIZE_T size = 0;
    SIZE_T valid = 0;
    LARGE_INTEGER now;
    TR_READER reader;
    ULONG before;
    ULONG i;
    int records;

    TrsPersist = !!PhGetIntegerSetting(SETTING_NAME_PERSIST);
    TrsFileName = SystemInformer_GetPluginDataFileName(TRS_FILE_NAME); // NULL with -nosettings: memory only

    if (!TrsFileName)
    {
        TrsPersist = FALSE;
        return;
    }

    if (!TrsPersist)
        return;

    buffer = TrsReadFile(&size);

    if (!buffer)
    {
        TrsRewriteFile(); // fresh header
        return;
    }

    memset(&reader, 0, sizeof(reader));
    reader.String = TrsOnString;
    reader.Sample = TrsOnSample;
    records = TrParse(buffer, size, &reader, &valid);
    PhFree(buffer);

    if (records < 0)
    {
        // not our file (or another version): start fresh
        PhClearList(TrsNames);
        TrsRewriteFile();
        return;
    }

    PhQuerySystemTime(&now);
    before = TrsSamples->Count;
    TrsThinAndPrune(now.QuadPart);

    // seed the leak tracker with what survived so a restart does not forget the recent growth
    for (i = 0; i < TrsSamples->Count; i++)
        TrsTrackerSeed(TrsSamples->Items[i], TRUE);

    if (valid != size || TrsSamples->Count != before || TrsStaleBytes)
    {
        TrsRewriteFile();
    }
    else if (TrsOpenFile(FALSE))
    {
        TrsFilePosition.QuadPart = (LONGLONG)valid;
        TrsStringsWritten = TrsNames->Count;
    }
}

VOID TrsStoreFlush(
    VOID
    )
{
    TrsCloseFile();
}

VOID TrsStoreClear(
    VOID
    )
{
    ULONG i;

    for (i = 0; i < TrsSamples->Count; i++)
        TrsFreeSample(TrsSamples->Items[i]);

    PhClearList(TrsSamples);

    for (i = 0; i < TrsNames->Count; i++)
        PhDereferenceObject(TrsNames->Items[i]);

    PhClearList(TrsNames);
    TrsStaleBytes = 0;
    TrsRewriteFile();
}
