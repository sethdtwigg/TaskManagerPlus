/*
 * TaskManagerPlus: interface exported by the ProcessHistory plugin so that other
 * plugins (ProcessRules) can read processes that started and exited between two
 * refreshes of the process list. The data comes from a local ETW session on
 * Microsoft-Windows-Kernel-Process (no driver, no network).
 *
 * Usage:
 *   PPROCESS_CAPTURE_INTERFACE intf = PhGetPluginInterfaceZ(PROCESS_CAPTURE_PLUGIN_NAME, PROCESS_CAPTURE_INTERFACE_VERSION);
 *   ULONG64 cursor = intf->GetCursor();            // once
 *   n = intf->ReadCompleted(&cursor, buffer, max); // every refresh
 */

#ifndef _PROCESSCAPTUREINTF_H
#define _PROCESSCAPTUREINTF_H

#define PROCESS_CAPTURE_PLUGIN_NAME L"TaskManagerPlus.ProcessHistory"
#define PROCESS_CAPTURE_INTERFACE_VERSION 1

#define PROCESS_CAPTURE_IMAGE_CHARS 260

/** A process whose whole life (start and stop) was observed by the ETW session. */
typedef struct _PROCESS_CAPTURE_RECORD
{
    ULONG ProcessId;
    ULONG ParentProcessId;
    ULONG SessionId;
    ULONG ExitCode;
    LONGLONG CreateTime; // system time (UTC, 100 ns), same clock as PH_PROCESS_ITEM.CreateTime
    LONGLONG ExitTime;
    BOOLEAN SeenByProcessList; // the process list had already shown this process while it ran
    BOOLEAN Reserved[7];
    WCHAR ImagePath[PROCESS_CAPTURE_IMAGE_CHARS]; // NT path from the start event (or the short name), NUL-terminated
} PROCESS_CAPTURE_RECORD, *PPROCESS_CAPTURE_RECORD;

typedef BOOLEAN (NTAPI *PPROCESS_CAPTURE_IS_ACTIVE)(
    VOID
    );

typedef ULONG64 (NTAPI *PPROCESS_CAPTURE_GET_CURSOR)(
    VOID
    );

/** Copies records completed after *Cursor (oldest first) and advances *Cursor. Returns the number copied. */
typedef ULONG (NTAPI *PPROCESS_CAPTURE_READ_COMPLETED)(
    _Inout_ PULONG64 Cursor,
    _Out_writes_(MaximumCount) PPROCESS_CAPTURE_RECORD Buffer,
    _In_ ULONG MaximumCount
    );

typedef struct _PROCESS_CAPTURE_INTERFACE
{
    ULONG Version; // must be first (PhGetPluginInterface checks it)
    PPROCESS_CAPTURE_IS_ACTIVE IsActive;
    PPROCESS_CAPTURE_GET_CURSOR GetCursor;
    PPROCESS_CAPTURE_READ_COMPLETED ReadCompleted;
} PROCESS_CAPTURE_INTERFACE, *PPROCESS_CAPTURE_INTERFACE;

#endif
