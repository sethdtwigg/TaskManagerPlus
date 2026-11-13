/*
 * TaskManagerPlus Process History plugin: short-lived process capture.
 *
 * Processes that start and exit between two refreshes of the process list are
 * invisible to the process provider. This file runs a private real-time ETW
 * session on Microsoft-Windows-Kernel-Process (process start/stop events, no
 * driver, local only), pairs the start and stop event of each process and keeps
 * the finished ones in a small ring buffer. ProcessHistory adds them to its
 * snapshots and ProcessRules reads them through PROCESS_CAPTURE_INTERFACE.
 *
 * Creating an ETW session needs administrator rights (or membership in
 * "Performance Log Users"). Without them the capture stays off and says so once
 * in the log. The setting TaskManagerPlus.ProcessHistory.CaptureShortLived turns
 * it off completely.
 */

#include "processhistory.h"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <processcaptureintf.h>

#define CAP_SESSION_NAME L"TaskManagerPlus-ProcessCapture"
#define CAP_KEYWORD_PROCESS 0x10ULL // WINEVENT_KEYWORD_PROCESS
#define CAP_EVENT_PROCESS_START 1
#define CAP_EVENT_PROCESS_STOP 2
#define CAP_INFLIGHT_COUNT 512
#define CAP_RING_COUNT 512
#define CAP_SEEN_COUNT 4096
#define CAP_SELFTEST_PID 0x7FFFF000

// {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716} Microsoft-Windows-Kernel-Process
static CONST GUID CapProviderGuid = { 0x22FB2CD6, 0x0E7B, 0x422B, { 0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16 } };

typedef struct _CAP_INFLIGHT
{
    BOOLEAN Used;
    ULONG ProcessId;
    ULONG ParentProcessId;
    ULONG SessionId;
    LONGLONG CreateTime;
    WCHAR ImagePath[PROCESS_CAPTURE_IMAGE_CHARS];
} CAP_INFLIGHT, *PCAP_INFLIGHT;

typedef struct _CAP_SEEN
{
    ULONG ProcessId;
    LONGLONG CreateTime;
} CAP_SEEN, *PCAP_SEEN;

static PH_QUEUED_LOCK CapLock = PH_QUEUED_LOCK_INIT;
static CAP_INFLIGHT CapInflight[CAP_INFLIGHT_COUNT];
static ULONG CapInflightVictim = 0;
static PROCESS_CAPTURE_RECORD CapRing[CAP_RING_COUNT];
static ULONG64 CapHead = 0; // number of records ever written
static CAP_SEEN CapSeen[CAP_SEEN_COUNT];
static ULONG CapSeenNext = 0;

static volatile BOOLEAN CapRunning = FALSE;
static BOOLEAN CapDenied = FALSE; // start failed; do not retry until the setting is toggled
static TRACEHANDLE CapSessionHandle = 0;
static HANDLE CapThreadHandle = NULL;

static VOID CapLog(
    _In_ PPH_STRING Text
    )
{
    PPH_STRING logText = PhConcatStrings2(L"Process capture: ", PhGetString(Text));

    PhLogMessageEntry(PH_LOG_ENTRY_MESSAGE, logText);
    PhDereferenceObject(logText);
    PhDereferenceObject(Text);
}

static BOOLEAN NTAPI CapIsActive(
    VOID
    )
{
    return CapRunning;
}

static ULONG64 NTAPI CapGetCursor(
    VOID
    )
{
    ULONG64 head;

    PhAcquireQueuedLockShared(&CapLock);
    head = CapHead;
    PhReleaseQueuedLockShared(&CapLock);

    return head;
}

static BOOLEAN CapWasSeenLocked(
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    )
{
    for (ULONG i = 0; i < CAP_SEEN_COUNT; i++)
    {
        if (CapSeen[i].ProcessId == ProcessId && CapSeen[i].CreateTime == CreateTime)
            return TRUE;
    }

    return FALSE;
}

static ULONG NTAPI CapReadCompleted(
    _Inout_ PULONG64 Cursor,
    _Out_writes_(MaximumCount) PPROCESS_CAPTURE_RECORD Buffer,
    _In_ ULONG MaximumCount
    )
{
    ULONG64 first;
    ULONG count = 0;

    PhAcquireQueuedLockShared(&CapLock);

    first = *Cursor;

    if (CapHead > CAP_RING_COUNT && first < CapHead - CAP_RING_COUNT)
        first = CapHead - CAP_RING_COUNT; // the consumer lagged; the oldest records are gone

    while (first < CapHead && count < MaximumCount)
    {
        Buffer[count] = CapRing[first % CAP_RING_COUNT];
        Buffer[count].SeenByProcessList = CapWasSeenLocked(Buffer[count].ProcessId, Buffer[count].CreateTime);
        count++;
        first++;
    }

    *Cursor = first;
    PhReleaseQueuedLockShared(&CapLock);

    return count;
}

PROCESS_CAPTURE_INTERFACE CapInterface =
{
    PROCESS_CAPTURE_INTERFACE_VERSION,
    CapIsActive,
    CapGetCursor,
    CapReadCompleted
};

/**
 * Process provider thread: a process was added to the process list. Remembered
 * so that consumers can tell which captured processes the list had already shown.
 */
VOID CapMarkSeen(
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime
    )
{
    PhAcquireQueuedLockExclusive(&CapLock);
    CapSeen[CapSeenNext].ProcessId = ProcessId;
    CapSeen[CapSeenNext].CreateTime = CreateTime;
    CapSeenNext = (CapSeenNext + 1) % CAP_SEEN_COUNT;
    PhReleaseQueuedLockExclusive(&CapLock);
}

static VOID CapOnProcessStart(
    _In_ ULONG ProcessId,
    _In_ ULONG ParentProcessId,
    _In_ ULONG SessionId,
    _In_ LONGLONG CreateTime,
    _In_ PCWSTR ImagePath
    )
{
    PCAP_INFLIGHT slot = NULL;

    PhAcquireQueuedLockExclusive(&CapLock);

    for (ULONG i = 0; i < CAP_INFLIGHT_COUNT; i++)
    {
        if (CapInflight[i].Used && CapInflight[i].ProcessId == ProcessId)
        {
            slot = &CapInflight[i]; // stale entry for a reused pid
            break;
        }
    }

    if (!slot)
    {
        for (ULONG i = 0; i < CAP_INFLIGHT_COUNT; i++)
        {
            if (!CapInflight[i].Used)
            {
                slot = &CapInflight[i];
                break;
            }
        }
    }

    if (!slot)
    {
        slot = &CapInflight[CapInflightVictim];
        CapInflightVictim = (CapInflightVictim + 1) % CAP_INFLIGHT_COUNT;
    }

    slot->Used = TRUE;
    slot->ProcessId = ProcessId;
    slot->ParentProcessId = ParentProcessId;
    slot->SessionId = SessionId;
    slot->CreateTime = CreateTime;
    wcsncpy_s(slot->ImagePath, RTL_NUMBER_OF(slot->ImagePath), ImagePath, _TRUNCATE);

    PhReleaseQueuedLockExclusive(&CapLock);
}

static VOID CapOnProcessStop(
    _In_ ULONG ProcessId,
    _In_ LONGLONG CreateTime,
    _In_ LONGLONG ExitTime,
    _In_ ULONG ExitCode,
    _In_ PCWSTR ShortName
    )
{
    PhAcquireQueuedLockExclusive(&CapLock);

    for (ULONG i = 0; i < CAP_INFLIGHT_COUNT; i++)
    {
        PCAP_INFLIGHT slot = &CapInflight[i];

        if (slot->Used && slot->ProcessId == ProcessId && (!CreateTime || slot->CreateTime == CreateTime))
        {
            PPROCESS_CAPTURE_RECORD record = &CapRing[CapHead % CAP_RING_COUNT];

            memset(record, 0, sizeof(PROCESS_CAPTURE_RECORD));
            record->ProcessId = ProcessId;
            record->ParentProcessId = slot->ParentProcessId;
            record->SessionId = slot->SessionId;
            record->ExitCode = ExitCode;
            record->CreateTime = slot->CreateTime;
            record->ExitTime = ExitTime;
            wcsncpy_s(
                record->ImagePath,
                RTL_NUMBER_OF(record->ImagePath),
                slot->ImagePath[0] ? slot->ImagePath : ShortName,
                _TRUNCATE
                );
            CapHead++;

            slot->Used = FALSE;
            break;
        }
    }

    PhReleaseQueuedLockExclusive(&CapLock);
}

// TDH: read one named property of a manifest-based event.

static BOOLEAN CapGetProperty(
    _In_ PEVENT_RECORD EventRecord,
    _In_ PCWSTR Name,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ ULONG Size
    )
{
    PROPERTY_DATA_DESCRIPTOR descriptor;

    descriptor.PropertyName = (ULONGLONG)(ULONG_PTR)Name;
    descriptor.ArrayIndex = ULONG_MAX;
    descriptor.Reserved = 0;

    return TdhGetProperty(EventRecord, 0, NULL, 1, &descriptor, Size, Buffer) == ERROR_SUCCESS;
}

static BOOLEAN CapGetString(
    _In_ PEVENT_RECORD EventRecord,
    _In_ PCWSTR Name,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ ULONG Count
    )
{
    PROPERTY_DATA_DESCRIPTOR descriptor;
    ULONG size = 0;
    PWSTR temporary;

    Buffer[0] = UNICODE_NULL;

    descriptor.PropertyName = (ULONGLONG)(ULONG_PTR)Name;
    descriptor.ArrayIndex = ULONG_MAX;
    descriptor.Reserved = 0;

    if (TdhGetPropertySize(EventRecord, 0, NULL, 1, &descriptor, &size) != ERROR_SUCCESS || size < sizeof(WCHAR))
        return FALSE;

    temporary = PhAllocateZero(size + sizeof(WCHAR));

    if (TdhGetProperty(EventRecord, 0, NULL, 1, &descriptor, size, (PBYTE)temporary) == ERROR_SUCCESS)
        wcsncpy_s(Buffer, Count, temporary, _TRUNCATE);

    PhFree(temporary);

    return Buffer[0] != UNICODE_NULL;
}

/** ETW consumer thread. Runs for every event of the session; keep it cheap. */
static VOID WINAPI CapEventRecordCallback(
    _In_ PEVENT_RECORD EventRecord
    )
{
    ULONG processId = 0;
    ULONG parentProcessId = 0;
    ULONG sessionId = 0;
    ULONG exitCode = 0;
    ULONGLONG createTime = 0;
    ULONGLONG exitTime = 0;
    WCHAR image[PROCESS_CAPTURE_IMAGE_CHARS];

    if (!IsEqualGUID(&EventRecord->EventHeader.ProviderId, &CapProviderGuid))
        return;

    switch (EventRecord->EventHeader.EventDescriptor.Id)
    {
    case CAP_EVENT_PROCESS_START:
        if (!CapGetProperty(EventRecord, L"ProcessID", &processId, sizeof(ULONG)))
            return;

        CapGetProperty(EventRecord, L"ParentProcessID", &parentProcessId, sizeof(ULONG));
        CapGetProperty(EventRecord, L"SessionID", &sessionId, sizeof(ULONG));
        CapGetProperty(EventRecord, L"CreateTime", &createTime, sizeof(ULONGLONG));
        CapGetString(EventRecord, L"ImageName", image, RTL_NUMBER_OF(image));

        if (!createTime)
            createTime = EventRecord->EventHeader.TimeStamp.QuadPart;

        CapOnProcessStart(processId, parentProcessId, sessionId, (LONGLONG)createTime, image);
        break;
    case CAP_EVENT_PROCESS_STOP:
        if (!CapGetProperty(EventRecord, L"ProcessID", &processId, sizeof(ULONG)))
            return;

        CapGetProperty(EventRecord, L"CreateTime", &createTime, sizeof(ULONGLONG));
        CapGetProperty(EventRecord, L"ExitTime", &exitTime, sizeof(ULONGLONG));
        CapGetProperty(EventRecord, L"ExitCode", &exitCode, sizeof(ULONG));
        CapGetString(EventRecord, L"ImageName", image, RTL_NUMBER_OF(image));

        if (!exitTime)
            exitTime = EventRecord->EventHeader.TimeStamp.QuadPart;

        CapOnProcessStop(processId, (LONGLONG)createTime, (LONGLONG)exitTime, exitCode, image);
        break;
    }
}

static NTSTATUS NTAPI CapTraceThread(
    _In_ PVOID Context
    )
{
    EVENT_TRACE_LOGFILEW logFile;
    TRACEHANDLE trace;

    memset(&logFile, 0, sizeof(logFile));
    logFile.LoggerName = (PWSTR)CAP_SESSION_NAME;
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = CapEventRecordCallback;

    trace = OpenTraceW(&logFile);

    if (trace != INVALID_PROCESSTRACE_HANDLE)
    {
        ProcessTrace(&trace, 1, NULL, NULL);
        CloseTrace(trace);
    }

    return STATUS_SUCCESS;
}

static PEVENT_TRACE_PROPERTIES CapCreateProperties(
    VOID
    )
{
    ULONG size = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(CAP_SESSION_NAME);
    PEVENT_TRACE_PROPERTIES properties = PhAllocateZero(size);

    properties->Wnode.BufferSize = size;
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1; // QPC timestamps
    properties->BufferSize = 16; // KB
    properties->MinimumBuffers = 2;
    properties->MaximumBuffers = 16;
    properties->FlushTimer = 1; // seconds; the lowest latency a real-time session offers
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    return properties;
}

static VOID CapStopSession(
    VOID
    )
{
    if (CapSessionHandle)
    {
        PEVENT_TRACE_PROPERTIES properties = CapCreateProperties();

        ControlTraceW(CapSessionHandle, NULL, properties, EVENT_TRACE_CONTROL_STOP);
        PhFree(properties);
        CapSessionHandle = 0;
    }

    if (CapThreadHandle)
    {
        LARGE_INTEGER timeout;

        timeout.QuadPart = -3 * (LONGLONG)PH_TIMEOUT_SEC;
        NtWaitForSingleObject(CapThreadHandle, FALSE, &timeout); // ProcessTrace returns once the session is stopped
        NtClose(CapThreadHandle);
        CapThreadHandle = NULL;
    }

    CapRunning = FALSE;
}

static VOID CapStartSession(
    VOID
    )
{
    ULONG status;
    PEVENT_TRACE_PROPERTIES properties;

    properties = CapCreateProperties();
    status = StartTraceW(&CapSessionHandle, CAP_SESSION_NAME, properties);

    if (status == ERROR_ALREADY_EXISTS)
    {
        // A session from a previous run that crashed: stop it by name and retry once.
        PEVENT_TRACE_PROPERTIES stale = CapCreateProperties();

        ControlTraceW(0, CAP_SESSION_NAME, stale, EVENT_TRACE_CONTROL_STOP);
        PhFree(stale);

        PhFree(properties);
        properties = CapCreateProperties();
        status = StartTraceW(&CapSessionHandle, CAP_SESSION_NAME, properties);
    }

    PhFree(properties);

    if (status != ERROR_SUCCESS)
    {
        CapSessionHandle = 0;
        CapDenied = TRUE;

        if (status == ERROR_ACCESS_DENIED)
        {
            CapLog(PhCreateString(
                L"short-lived process capture is off because it needs administrator rights "
                L"(an ETW session cannot be created). Run elevated, or turn the option off in Options > Process History."
                ));
        }
        else
        {
            CapLog(PhFormatString(L"short-lived process capture could not start (error %lu).", status));
        }

        return;
    }

    status = EnableTraceEx2(
        CapSessionHandle,
        &CapProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        CAP_KEYWORD_PROCESS,
        0,
        0,
        NULL
        );

    if (status != ERROR_SUCCESS)
    {
        CapStopSession();
        CapDenied = TRUE;
        CapLog(PhFormatString(L"could not enable the Kernel-Process provider (error %lu).", status));
        return;
    }

    CapThreadHandle = PhCreateThread(0, CapTraceThread, NULL);

    if (!CapThreadHandle)
    {
        CapStopSession();
        CapDenied = TRUE;
        CapLog(PhCreateString(L"could not create the ETW consumer thread."));
        return;
    }

    CapRunning = TRUE;

    CapLog(PhCreateString(L"started (Microsoft-Windows-Kernel-Process, local ETW session)."));
}

/** Injects a synthetic short-lived process through the same path ETW events take (test hook). */
static VOID CapInjectSelfTest(
    VOID
    )
{
    LARGE_INTEGER now;
    LONGLONG start;
    LONGLONG stop;

    PhQuerySystemTime(&now);
    start = now.QuadPart - 20 * PH_TICKS_PER_MS;
    stop = now.QuadPart - 5 * PH_TICKS_PER_MS;

    CapOnProcessStart(
        CAP_SELFTEST_PID,
        HandleToUlong(NtCurrentProcessId()),
        0,
        start,
        L"\\Device\\HarddiskVolume1\\TaskManagerPlus\\selftest\\tmp-selftest.exe"
        );
    CapOnProcessStop(CAP_SELFTEST_PID, start, stop, 0, L"tmp-selftest.exe");

    CapLog(PhCreateString(L"self-test: injected a synthetic short-lived process (tmp-selftest.exe)."));
}

/** Main thread, once per refresh: follows the setting and runs the test hook. */
VOID CapUpdate(
    VOID
    )
{
    BOOLEAN enabled = !!PhGetIntegerSetting(SETTING_NAME_CAPTURE);

    if (enabled && !CapRunning && !CapDenied)
    {
        CapStartSession();
    }
    else if (!enabled)
    {
        if (CapRunning)
        {
            CapStopSession();
            CapLog(PhCreateString(L"stopped (turned off in the options)."));
        }

        CapDenied = FALSE;
    }

    if (PhGetIntegerSetting(SETTING_NAME_CAPTURE_SELF_TEST))
    {
        PhSetIntegerSetting(SETTING_NAME_CAPTURE_SELF_TEST, 0);
        CapInjectSelfTest();
    }
}

VOID CapShutdown(
    VOID
    )
{
    if (CapRunning || CapSessionHandle)
        CapStopSession();
}
