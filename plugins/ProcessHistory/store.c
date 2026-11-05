/*
 * TaskManagerPlus Process History plugin: snapshot store.
 *
 * Everything in this file runs on the main thread (the collector is driven by
 * GeneralCallbackProcessesUpdated and the tab talks to the store from window
 * messages), so no locking is needed.
 *
 * On-disk format (tmp-history.bin, little endian, 4-byte packed):
 *
 *   HIST_FILE_HEADER   { ULONG Magic = 'TMPH'; ULONG Version = 1; ULONG Reserved[2]; }
 *   repeated:
 *   HIST_RECORD_HEADER { ULONG Type; ULONG Length; } followed by Length payload bytes
 *     Type 1 (string):   UTF-16 characters, no terminator. String table index is
 *                        the position of the record among all string records.
 *     Type 2 (snapshot): HIST_SNAPSHOT_RECORD { LONGLONG Time; ULONG Count; ULONG Reserved; }
 *                        followed by Count HIST_ENTRY structures sorted by
 *                        (ProcessId, CreateTime).
 *
 * The file is append-only. Unknown record types are skipped; a truncated
 * trailing record is ignored. When more than half of the file is older than
 * the retention period it is rewritten compacted.
 */

#include "processhistory.h"

#define HIST_FILE_MAGIC 0x48504D54 // 'TMPH'
#define HIST_FILE_VERSION 1
#define HIST_RECORD_STRING 1
#define HIST_RECORD_SNAPSHOT 2
#define HIST_MAX_FILE_SIZE (512ULL * 1024 * 1024)
#define HIST_COMPACT_MIN_SIZE (64ULL * 1024)
#define HIST_READ_CHUNK (16 * 1024 * 1024)

#include <pshpack4.h>
typedef struct _HIST_FILE_HEADER
{
    ULONG Magic;
    ULONG Version;
    ULONG Reserved[2];
} HIST_FILE_HEADER, *PHIST_FILE_HEADER;

typedef struct _HIST_RECORD_HEADER
{
    ULONG Type;
    ULONG Length;
} HIST_RECORD_HEADER, *PHIST_RECORD_HEADER;

typedef struct _HIST_SNAPSHOT_RECORD
{
    LONGLONG Time;
    ULONG Count;
    ULONG Reserved;
} HIST_SNAPSHOT_RECORD, *PHIST_SNAPSHOT_RECORD;
#include <poppack.h>

typedef struct _HIST_STRING_ENTRY
{
    PH_STRINGREF String; // points into the PPH_STRING held by the string list
    ULONG Index;
} HIST_STRING_ENTRY, *PHIST_STRING_ENTRY;

static PPH_LIST HistSnapshotList = NULL; // PHIST_SNAPSHOT, ordered by time
static PPH_LIST HistStringList = NULL; // PPH_STRING, position = index
static PPH_HASHTABLE HistStringTable = NULL; // HIST_STRING_ENTRY
static ULONG HistStringsWritten = 0; // string list entries already on disk
static PPH_STRING HistFileName = NULL;
static HANDLE HistFileHandle = NULL;
static LARGE_INTEGER HistFilePosition = { 0 };
static ULONG64 HistStaleBytes = 0;
static LARGE_INTEGER HistLastSnapshotTime = { 0 };
static BOOLEAN HistPersistEnabled = FALSE;

static BOOLEAN NTAPI HistStringEqualFunction(
    _In_ PVOID Entry1,
    _In_ PVOID Entry2
    )
{
    PHIST_STRING_ENTRY entry1 = Entry1;
    PHIST_STRING_ENTRY entry2 = Entry2;

    return PhEqualStringRef(&entry1->String, &entry2->String, FALSE);
}

static ULONG NTAPI HistStringHashFunction(
    _In_ PVOID Entry
    )
{
    PHIST_STRING_ENTRY entry = Entry;

    return PhHashStringRefEx(&entry->String, FALSE, PH_STRING_HASH_XXH32);
}

static PPH_HASHTABLE HistCreateStringTable(
    VOID
    )
{
    return PhCreateHashtable(
        sizeof(HIST_STRING_ENTRY),
        HistStringEqualFunction,
        HistStringHashFunction,
        256
        );
}

/**
 * Appends a string to a list/table pair without checking for duplicates
 * (used when loading, where indices are positional) and registers it in the
 * table if no equal string is registered yet.
 */
static ULONG HistAppendString(
    _In_ PPH_LIST List,
    _In_ PPH_HASHTABLE Table,
    _In_ PPH_STRING String
    )
{
    HIST_STRING_ENTRY entry;

    PhAddItemList(List, String);

    entry.String = String->sr;
    entry.Index = List->Count - 1;
    PhAddEntryHashtableEx(Table, &entry, NULL);

    return entry.Index;
}

/**
 * Returns the index of a string in a list/table pair, adding a copy if needed.
 */
static ULONG HistInternStringEx(
    _In_ PPH_LIST List,
    _In_ PPH_HASHTABLE Table,
    _In_ PCPH_STRINGREF String
    )
{
    HIST_STRING_ENTRY lookup;
    PHIST_STRING_ENTRY entry;

    lookup.String = *String;
    lookup.Index = 0;

    if (entry = PhFindEntryHashtable(Table, &lookup))
        return entry->Index;

    return HistAppendString(List, Table, PhCreateString2(String));
}

static ULONG HistInternString(
    _In_ PCPH_STRINGREF String
    )
{
    return HistInternStringEx(HistStringList, HistStringTable, String);
}

static int __cdecl HistEntryCompare(
    _In_ void const* Element1,
    _In_ void const* Element2
    )
{
    PHIST_ENTRY entry1 = (PHIST_ENTRY)Element1;
    PHIST_ENTRY entry2 = (PHIST_ENTRY)Element2;

    if (entry1->ProcessId != entry2->ProcessId)
        return entry1->ProcessId < entry2->ProcessId ? -1 : 1;
    if (entry1->CreateTime != entry2->CreateTime)
        return entry1->CreateTime < entry2->CreateTime ? -1 : 1;

    return 0;
}

static VOID HistFreeStrings(
    _Inout_ PPH_LIST List
    )
{
    for (ULONG i = 0; i < List->Count; i++)
        PhDereferenceObject(List->Items[i]);

    PhDereferenceObject(List);
}

static VOID HistCloseFile(
    VOID
    )
{
    if (HistFileHandle)
    {
        NtClose(HistFileHandle);
        HistFileHandle = NULL;
    }
}

static BOOLEAN HistWriteBytes(
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length
    )
{
    NTSTATUS status;
    ULONG written = 0;

    if (!HistFileHandle)
        return FALSE;

    status = PhWriteFile(HistFileHandle, Buffer, Length, &HistFilePosition, &written);

    if (!NT_SUCCESS(status) || written != Length)
    {
        // Give up on persistence for this session rather than corrupting the file.
        HistCloseFile();
        return FALSE;
    }

    HistFilePosition.QuadPart += Length;
    return TRUE;
}

/**
 * Opens (or recreates) the history file for appending.
 *
 * \param Overwrite TRUE to truncate the file and write a fresh header.
 */
static BOOLEAN HistOpenFile(
    _In_ BOOLEAN Overwrite
    )
{
    NTSTATUS status;

    HistCloseFile();

    if (!HistFileName)
        return FALSE;

    PhCreateDirectoryFullPathWin32(&HistFileName->sr);

    status = PhCreateFileWin32(
        &HistFileHandle,
        HistFileName->Buffer,
        FILE_GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        Overwrite ? FILE_OVERWRITE_IF : FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
        );

    if (!NT_SUCCESS(status))
    {
        HistFileHandle = NULL;
        return FALSE;
    }

    if (Overwrite)
    {
        HIST_FILE_HEADER header;

        memset(&header, 0, sizeof(HIST_FILE_HEADER));
        header.Magic = HIST_FILE_MAGIC;
        header.Version = HIST_FILE_VERSION;

        HistFilePosition.QuadPart = 0;
        HistStaleBytes = 0;

        if (!HistWriteBytes(&header, sizeof(HIST_FILE_HEADER)))
            return FALSE;
    }

    return TRUE;
}

/**
 * Writes string table entries that are not on disk yet.
 */
static BOOLEAN HistWritePendingStrings(
    VOID
    )
{
    while (HistStringsWritten < HistStringList->Count)
    {
        PPH_STRING string = HistStringList->Items[HistStringsWritten];
        HIST_RECORD_HEADER header;

        header.Type = HIST_RECORD_STRING;
        header.Length = (ULONG)string->Length;

        if (!HistWriteBytes(&header, sizeof(HIST_RECORD_HEADER)))
            return FALSE;
        if (header.Length && !HistWriteBytes(string->Buffer, header.Length))
            return FALSE;

        HistStringsWritten++;
    }

    return TRUE;
}

static BOOLEAN HistWriteSnapshot(
    _Inout_ PHIST_SNAPSHOT Snapshot
    )
{
    HIST_RECORD_HEADER header;
    HIST_SNAPSHOT_RECORD record;
    ULONG entriesLength;

    if (!HistWritePendingStrings())
        return FALSE;

    entriesLength = Snapshot->Count * sizeof(HIST_ENTRY);

    header.Type = HIST_RECORD_SNAPSHOT;
    header.Length = sizeof(HIST_SNAPSHOT_RECORD) + entriesLength;
    record.Time = Snapshot->Time.QuadPart;
    record.Count = Snapshot->Count;
    record.Reserved = 0;

    if (!HistWriteBytes(&header, sizeof(HIST_RECORD_HEADER)))
        return FALSE;
    if (!HistWriteBytes(&record, sizeof(HIST_SNAPSHOT_RECORD)))
        return FALSE;
    if (entriesLength && !HistWriteBytes(Snapshot->Entries, entriesLength))
        return FALSE;

    Snapshot->FileBytes = sizeof(HIST_RECORD_HEADER) + header.Length;

    return TRUE;
}

/**
 * Rebuilds the string table so it only holds strings referenced by retained
 * snapshots, then rewrites the whole file from the in-memory state.
 */
static VOID HistRewriteFile(
    VOID
    )
{
    PPH_LIST newList;
    PPH_HASHTABLE newTable;
    PULONG remap;
    ULONG oldCount;
    ULONG emptyIndex;
    PH_STRINGREF empty;

    oldCount = HistStringList->Count;
    newList = PhCreateList(oldCount ? oldCount : 16);
    newTable = HistCreateStringTable();
    remap = PhAllocate(sizeof(ULONG) * (oldCount ? oldCount : 1));
    memset(remap, 0xff, sizeof(ULONG) * (oldCount ? oldCount : 1));
    PhInitializeEmptyStringRef(&empty);
    emptyIndex = HistInternStringEx(newList, newTable, &empty);

    for (ULONG i = 0; i < HistSnapshotList->Count; i++)
    {
        PHIST_SNAPSHOT snapshot = HistSnapshotList->Items[i];

        for (ULONG j = 0; j < snapshot->Count; j++)
        {
            PHIST_ENTRY entry = &snapshot->Entries[j];
            PULONG indices[2] = { &entry->NameIndex, &entry->PathIndex };

            for (ULONG k = 0; k < RTL_NUMBER_OF(indices); k++)
            {
                ULONG oldIndex = *indices[k];

                if (oldIndex >= oldCount)
                {
                    *indices[k] = emptyIndex;
                    continue;
                }

                if (remap[oldIndex] == ULONG_MAX)
                {
                    PPH_STRING oldString = HistStringList->Items[oldIndex];
                    remap[oldIndex] = HistInternStringEx(newList, newTable, &oldString->sr);
                }

                *indices[k] = remap[oldIndex];
            }
        }
    }

    PhFree(remap);
    HistFreeStrings(HistStringList);
    PhDereferenceObject(HistStringTable);
    HistStringList = newList;
    HistStringTable = newTable;
    HistStringsWritten = 0;

    if (!HistOpenFile(TRUE))
        return;

    for (ULONG i = 0; i < HistSnapshotList->Count; i++)
    {
        if (!HistWriteSnapshot(HistSnapshotList->Items[i]))
            return;
    }
}

static VOID HistMaybeCompact(
    VOID
    )
{
    if (!HistFileHandle)
        return;

    if ((ULONG64)HistFilePosition.QuadPart >= HIST_COMPACT_MIN_SIZE &&
        HistStaleBytes * 2 > (ULONG64)HistFilePosition.QuadPart)
    {
        HistRewriteFile();
    }
}

static LONGLONG HistGetRetentionCutoff(
    _In_ LARGE_INTEGER Now
    )
{
    return Now.QuadPart - (LONGLONG)HistGetRetentionHours() * 3600 * PH_TICKS_PER_SEC;
}

/**
 * Drops in-memory snapshots that are older than the retention period.
 */
static VOID HistPrune(
    _In_ LARGE_INTEGER Now
    )
{
    LONGLONG cutoff = HistGetRetentionCutoff(Now);
    ULONG remove = 0;

    while (remove < HistSnapshotList->Count)
    {
        PHIST_SNAPSHOT snapshot = HistSnapshotList->Items[remove];

        if (snapshot->Time.QuadPart >= cutoff)
            break;

        HistStaleBytes += snapshot->FileBytes;
        PhFree(snapshot);
        remove++;
    }

    if (remove)
        PhRemoveItemsList(HistSnapshotList, 0, remove);
}

ULONG HistGetIntervalSeconds(
    VOID
    )
{
    ULONG value = PhGetIntegerSetting(SETTING_NAME_INTERVAL_SECONDS);

    if (value < HIST_MIN_INTERVAL_SECONDS)
        value = HIST_MIN_INTERVAL_SECONDS;
    if (value > HIST_MAX_INTERVAL_SECONDS)
        value = HIST_MAX_INTERVAL_SECONDS;

    return value;
}

ULONG HistGetRetentionHours(
    VOID
    )
{
    ULONG value = PhGetIntegerSetting(SETTING_NAME_RETENTION_HOURS);

    if (value < HIST_MIN_RETENTION_HOURS)
        value = HIST_MIN_RETENTION_HOURS;
    if (value > HIST_MAX_RETENTION_HOURS)
        value = HIST_MAX_RETENTION_HOURS;

    return value;
}

VOID HistStoreInitialize(
    VOID
    )
{
    HistSnapshotList = PhCreateList(1024);
    HistStringList = PhCreateList(256);
    HistStringTable = HistCreateStringTable();
}

/**
 * Reads the whole history file into memory. Returns NULL (and leaves
 * *Size untouched) when the file is missing, empty, too large or unreadable.
 */
static PVOID HistReadFile(
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
        HistFileName->Buffer,
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
        (ULONG64)fileSize.QuadPart <= HIST_MAX_FILE_SIZE)
    {
        if (buffer = PhAllocateSafe((SIZE_T)fileSize.QuadPart))
        {
            while (offset < (SIZE_T)fileSize.QuadPart)
            {
                LARGE_INTEGER position;
                ULONG toRead;
                ULONG read = 0;

                position.QuadPart = offset;
                toRead = (ULONG)min((SIZE_T)fileSize.QuadPart - offset, HIST_READ_CHUNK);

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

VOID HistStoreLoad(
    VOID
    )
{
    PVOID buffer;
    SIZE_T size = 0;
    SIZE_T position;
    LARGE_INTEGER now;
    LONGLONG cutoff;
    ULONG64 staleBytes = 0;
    BOOLEAN headerValid = FALSE;

    HistPersistEnabled = !!PhGetIntegerSetting(SETTING_NAME_PERSIST_TO_DISK);

    if (SystemInformer_IsPortableMode())
        HistFileName = PhGetApplicationDirectoryFileNameZ(HIST_FILE_NAME, FALSE);
    else
        HistFileName = PhGetRoamingAppDataDirectoryZ(HIST_FILE_NAME, FALSE);

    if (!HistFileName)
    {
        HistPersistEnabled = FALSE;
        return;
    }

    if (!HistPersistEnabled)
        return;

    PhQuerySystemTime(&now);
    cutoff = HistGetRetentionCutoff(now);

    buffer = HistReadFile(&size);
    position = 0;

    if (buffer && size >= sizeof(HIST_FILE_HEADER))
    {
        PHIST_FILE_HEADER fileHeader = buffer;

        if (fileHeader->Magic == HIST_FILE_MAGIC && fileHeader->Version == HIST_FILE_VERSION)
        {
            headerValid = TRUE;
            position = sizeof(HIST_FILE_HEADER);

            while (position + sizeof(HIST_RECORD_HEADER) <= size)
            {
                PHIST_RECORD_HEADER recordHeader = PTR_ADD_OFFSET(buffer, position);
                PVOID payload = PTR_ADD_OFFSET(buffer, position + sizeof(HIST_RECORD_HEADER));
                SIZE_T recordBytes;

                if (recordHeader->Length > size - position - sizeof(HIST_RECORD_HEADER))
                    break; // truncated trailing record

                recordBytes = sizeof(HIST_RECORD_HEADER) + recordHeader->Length;

                if (recordHeader->Type == HIST_RECORD_STRING)
                {
                    if (recordHeader->Length & 1)
                        break; // corrupt

                    HistAppendString(HistStringList, HistStringTable, PhCreateStringEx(payload, recordHeader->Length));
                }
                else if (recordHeader->Type == HIST_RECORD_SNAPSHOT)
                {
                    PHIST_SNAPSHOT_RECORD record = payload;
                    PHIST_ENTRY entries;
                    BOOLEAN keep = TRUE;

                    if (recordHeader->Length < sizeof(HIST_SNAPSHOT_RECORD))
                        break; // corrupt
                    if ((SIZE_T)record->Count * sizeof(HIST_ENTRY) != recordHeader->Length - sizeof(HIST_SNAPSHOT_RECORD))
                        break; // corrupt

                    entries = PTR_ADD_OFFSET(payload, sizeof(HIST_SNAPSHOT_RECORD));

                    if (record->Time < cutoff)
                    {
                        keep = FALSE;
                    }
                    else
                    {
                        for (ULONG i = 0; i < record->Count; i++)
                        {
                            if (entries[i].NameIndex >= HistStringList->Count ||
                                entries[i].PathIndex >= HistStringList->Count)
                            {
                                keep = FALSE; // dangling string reference
                                break;
                            }
                        }
                    }

                    if (keep)
                    {
                        PHIST_SNAPSHOT snapshot;

                        snapshot = PhAllocate(FIELD_OFFSET(HIST_SNAPSHOT, Entries) + (SIZE_T)record->Count * sizeof(HIST_ENTRY));
                        snapshot->Time.QuadPart = record->Time;
                        snapshot->Count = record->Count;
                        snapshot->FileBytes = (ULONG)recordBytes;
                        memcpy(snapshot->Entries, entries, (SIZE_T)record->Count * sizeof(HIST_ENTRY));
                        qsort(snapshot->Entries, snapshot->Count, sizeof(HIST_ENTRY), HistEntryCompare);

                        PhAddItemList(HistSnapshotList, snapshot);
                    }
                    else
                    {
                        staleBytes += recordBytes;
                    }
                }
                // Unknown record types are skipped for forward compatibility.

                position += recordBytes;
            }
        }
    }

    if (buffer)
        PhFree(buffer);

    HistStringsWritten = HistStringList->Count;

    if (!headerValid || position != size || (staleBytes * 2 > size && size >= HIST_COMPACT_MIN_SIZE))
    {
        // Missing, corrupt, truncated or mostly stale: start a fresh file from
        // whatever survived in memory.
        HistRewriteFile();
    }
    else
    {
        HistFilePosition.QuadPart = (LONGLONG)position;
        HistStaleBytes = staleBytes;
        HistOpenFile(FALSE);
    }
}

VOID HistStoreFlush(
    VOID
    )
{
    // Writes are synchronous; just release the handle.
    HistCloseFile();
}

/**
 * Applies a change of the persist setting made in the options dialog.
 */
static VOID HistSyncPersistSetting(
    VOID
    )
{
    BOOLEAN persist = !!PhGetIntegerSetting(SETTING_NAME_PERSIST_TO_DISK);

    if (!HistFileName)
        return;

    if (persist && !HistPersistEnabled)
    {
        HistPersistEnabled = TRUE;
        // Write everything we hold so the file reflects the in-memory history.
        HistRewriteFile();
    }
    else if (!persist && HistPersistEnabled)
    {
        HistPersistEnabled = FALSE;
        HistCloseFile();
    }
}

VOID HistStoreCollect(
    VOID
    )
{
    LARGE_INTEGER now;
    PPH_PROCESS_ITEM* processItems;
    ULONG numberOfProcessItems;
    PHIST_SNAPSHOT snapshot;
    PH_STRINGREF empty;

    PhQuerySystemTime(&now);

    HistSyncPersistSetting();

    if (HistLastSnapshotTime.QuadPart != 0 &&
        now.QuadPart - HistLastSnapshotTime.QuadPart < (LONGLONG)HistGetIntervalSeconds() * PH_TICKS_PER_SEC)
    {
        return;
    }

    PhInitializeEmptyStringRef(&empty);
    PhEnumProcessItems(&processItems, &numberOfProcessItems);

    snapshot = PhAllocateZero(FIELD_OFFSET(HIST_SNAPSHOT, Entries) + (SIZE_T)numberOfProcessItems * sizeof(HIST_ENTRY));
    snapshot->Time = now;

    for (ULONG i = 0; i < numberOfProcessItems; i++)
    {
        PPH_PROCESS_ITEM processItem = processItems[i];
        PHIST_ENTRY entry;
        PPH_STRING fileName;

        if (PH_IS_FAKE_PROCESS_ID(processItem->ProcessId))
            continue;

        entry = &snapshot->Entries[snapshot->Count++];
        entry->ProcessId = HandleToUlong(processItem->ProcessId);
        entry->ParentProcessId = HandleToUlong(processItem->ParentProcessId);
        entry->SessionId = processItem->SessionId;
        entry->NumberOfThreads = processItem->NumberOfThreads;
        entry->NumberOfHandles = processItem->NumberOfHandles;
        entry->CpuUsage = processItem->CpuUsage;
        entry->CreateTime = processItem->CreateTime.QuadPart;
        entry->PrivateBytes = processItem->VmCounters.PagefileUsage;
        entry->WorkingSetSize = processItem->VmCounters.WorkingSetSize;

        entry->NameIndex = HistInternString(processItem->ProcessName ? &processItem->ProcessName->sr : &empty);

        fileName = processItem->FileName ? PhGetFileName(processItem->FileName) : NULL;
        entry->PathIndex = HistInternString(fileName ? &fileName->sr : &empty);
        PhClearReference(&fileName);
    }

    PhDereferenceObjects(processItems, numberOfProcessItems);
    PhFree(processItems);

    qsort(snapshot->Entries, snapshot->Count, sizeof(HIST_ENTRY), HistEntryCompare);

    PhAddItemList(HistSnapshotList, snapshot);
    HistLastSnapshotTime = now;

    HistPrune(now);

    // The handle is acquired by HistStoreLoad / HistSyncPersistSetting /
    // HistStoreClear. If a write fails the handle is dropped and persistence
    // stops for this session; the partial trailing record is discarded on the
    // next load.
    if (HistPersistEnabled && HistFileHandle)
    {
        HistWriteSnapshot(snapshot);
        HistMaybeCompact();
    }

    HistTabSnapshotsChanged();
}

VOID HistStoreClear(
    VOID
    )
{
    for (ULONG i = 0; i < HistSnapshotList->Count; i++)
        PhFree(HistSnapshotList->Items[i]);
    PhClearList(HistSnapshotList);

    HistFreeStrings(HistStringList);
    PhDereferenceObject(HistStringTable);
    HistStringList = PhCreateList(256);
    HistStringTable = HistCreateStringTable();
    HistStringsWritten = 0;
    HistStaleBytes = 0;
    HistLastSnapshotTime.QuadPart = 0;

    HistCloseFile();

    if (HistFileName)
        PhDeleteFileWin32(HistFileName->Buffer);

    if (HistPersistEnabled)
        HistOpenFile(TRUE);

    HistTabSnapshotsChanged();
}

ULONG HistStoreGetSnapshotCount(
    VOID
    )
{
    return HistSnapshotList ? HistSnapshotList->Count : 0;
}

PHIST_SNAPSHOT HistStoreGetSnapshot(
    _In_ ULONG Index
    )
{
    if (!HistSnapshotList || Index >= HistSnapshotList->Count)
        return NULL;

    return HistSnapshotList->Items[Index];
}

PPH_STRING HistStoreGetString(
    _In_ ULONG Index
    )
{
    if (!HistStringList || Index >= HistStringList->Count)
        return NULL;

    return HistStringList->Items[Index];
}

PHIST_ENTRY HistSnapshotFindEntry(
    _In_ PHIST_SNAPSHOT Snapshot,
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    )
{
    ULONG low = 0;
    ULONG high = Snapshot->Count;

    while (low < high)
    {
        ULONG mid = low + (high - low) / 2;
        PHIST_ENTRY entry = &Snapshot->Entries[mid];
        int result;

        if (entry->ProcessId != ProcessId)
            result = entry->ProcessId < ProcessId ? -1 : 1;
        else if (entry->CreateTime != CreateTime)
            result = entry->CreateTime < CreateTime ? -1 : 1;
        else
            return entry;

        if (result < 0)
            low = mid + 1;
        else
            high = mid;
    }

    return NULL;
}
