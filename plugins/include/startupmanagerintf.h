/*
 * TaskManagerPlus: interface exported by the StartupManager plugin so that the QuickActions palette can list
 * startup items and enable or disable them without a copy of the enumeration code.
 *
 * Usage:
 *   PSTARTUP_MANAGER_INTERFACE intf = PhGetPluginInterfaceZ(STARTUP_MANAGER_PLUGIN_NAME, STARTUP_MANAGER_INTERFACE_VERSION);
 *   PPH_LIST entries = intf->Enumerate();      // list of PSTARTUP_ENTRY_INFO
 *   intf->SetEnabled(entry, FALSE);            // by entry identity (re-enumerates, then toggles)
 *   intf->FreeEntries(entries);
 */

#ifndef _STARTUPMANAGERINTF_H
#define _STARTUPMANAGERINTF_H

#define STARTUP_MANAGER_PLUGIN_NAME L"TaskManagerPlus.StartupManager"
#define STARTUP_MANAGER_INTERFACE_VERSION 1

typedef struct _STARTUP_ENTRY_INFO
{
    PPH_STRING Name; // display name
    PPH_STRING Command; // command line, may be NULL
    PPH_STRING FileName; // resolved executable (Win32 path), may be NULL
    PPH_STRING Type; // "Registry (HKCU)", "Startup folder", "Scheduled task" ...
    BOOLEAN Enabled;
    BOOLEAN CanToggle;

    // Identity used by SetEnabled.
    ULONG Source;
    PPH_STRING ObjectPath;
    PPH_STRING ApprovedValueName; // may be NULL

    // Appended for SessionReports / boot timing (read only).
    PCWSTR Impact; // static string: "Low", "Medium", "High", "Not measured"
    ULONG BootDelayMs; // measured boot delay from the Diagnostics-Performance log, 0 = none
} STARTUP_ENTRY_INFO, *PSTARTUP_ENTRY_INFO;

typedef struct _STARTUP_MANAGER_INTERFACE
{
    ULONG Version;
    PPH_LIST (NTAPI *Enumerate)(VOID); // main thread; caller frees with FreeEntries
    VOID (NTAPI *FreeEntries)(_In_ PPH_LIST Entries);
    NTSTATUS (NTAPI *SetEnabled)(_In_ PSTARTUP_ENTRY_INFO Entry, _In_ BOOLEAN Enable);
} STARTUP_MANAGER_INTERFACE, *PSTARTUP_MANAGER_INTERFACE;

#endif
