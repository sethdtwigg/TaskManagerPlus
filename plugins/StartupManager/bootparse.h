/*
 * TaskManagerPlus StartupManager: boot timing from the Microsoft-Windows-Diagnostics-Performance/Operational log.
 *
 * Pure C (no System Informer, no Win32) so tools\tests\unit can compile it on its own. The plugin renders each log event
 * to XML (EvtRender, EvtRenderEventXml) and feeds it here; tests feed it sample XML.
 *
 * Events used (Provider Microsoft-Windows-Diagnostics-Performance):
 *   100  boot performance monitoring: BootTime, MainPathBootTime, BootPostBootTime and the per-phase times (all ms)
 *   101  an application slowed the boot / logon: Name, FriendlyName, Path, TotalTime, DegradationTime (ms)
 *   102  a driver (same shape), 103 a service, 106 a device (PnP), 108 other component
 * Items are matched to startup entries by executable base name or by name.
 */

#ifndef _BP_BOOTPARSE_H_
#define _BP_BOOTPARSE_H_

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#define BP_MAX_FIELDS 72
#define BP_NAME_CHARS 64
#define BP_VALUE_CHARS 400
#define BP_ITEM_CHARS 260

typedef struct _BP_FIELD
{
    wchar_t Name[BP_NAME_CHARS];
    wchar_t Value[BP_VALUE_CHARS];
} BP_FIELD;

/** One event from its XML. Large (about 70 KB): allocate it, do not put it on the stack. */
typedef struct _BP_EVENT
{
    uint32_t EventId;
    int64_t Time; // FILETIME (UTC), 0 when absent
    uint32_t FieldCount;
    BP_FIELD Fields[BP_MAX_FIELDS];
} BP_EVENT;

typedef struct _BP_BOOT
{
    int64_t Time; // when the boot finished being measured (event time)
    uint32_t BootTimeMs;
    uint32_t MainPathBootTimeMs; // power on to desktop
    uint32_t PostBootTimeMs; // desktop to idle
    uint32_t OsLoaderMs;
    uint32_t KernelInitMs;
    uint32_t DriverInitMs;
    uint32_t DevicesInitMs;
    uint32_t SmssInitMs;
    uint32_t CriticalServicesInitMs;
    uint32_t UserProfileMs;
    uint32_t ExplorerInitMs;
    uint32_t UserLogonWaitMs;
    uint32_t NumStartupApps;
    uint32_t IsDegradation;
} BP_BOOT;

typedef struct _BP_ITEM
{
    uint32_t EventId; // 101 application, 102 driver, 103 service, 106 device, 108 other
    wchar_t Name[BP_ITEM_CHARS]; // e.g. "OneDrive.exe" or a service name
    wchar_t Friendly[BP_ITEM_CHARS];
    wchar_t Path[BP_ITEM_CHARS];
    uint32_t TotalMs;
    uint32_t DegradationMs;
    int64_t Time; // newest event for this item
    uint32_t Events; // how many events were seen for it
} BP_ITEM;

typedef struct _BP_STATE
{
    int HasBoot;
    BP_BOOT Boot; // the newest boot (event 100)
    uint32_t BootCount; // event 100 records seen
    BP_ITEM *Items; // malloc
    uint32_t ItemCount;
    uint32_t ItemCapacity;
    uint32_t EventsSeen;
    uint32_t EventsIgnored; // malformed or other ids
} BP_STATE;

#ifdef __cplusplus
extern "C" {
#endif

/** Parses one rendered event. Returns 1 when an EventID was found; fields are decoded (&amp; ...) and truncated to fit. */
int BpParseEventXml(const wchar_t *Xml, BP_EVENT *Out);

/** The value of a named field or NULL. */
const wchar_t *BpGetField(const BP_EVENT *Event, const wchar_t *Name);

/** Field as an unsigned number (decimal), or Default when absent or not a number. */
uint32_t BpGetUInt(const BP_EVENT *Event, const wchar_t *Name, uint32_t Default);

/** "2026-11-13T08:00:11.1234567Z" to a FILETIME; 0 when malformed. */
int64_t BpParseIsoTime(const wchar_t *Text);

void BpInit(BP_STATE *State);
void BpFree(BP_STATE *State);

/** Feeds one event: 100 updates the newest boot; 101/102/103/106/108 update the item list. Others are ignored. */
void BpAddEvent(BP_STATE *State, const BP_EVENT *Event);

/** Convenience: parse + add. Returns 1 when the XML was understood. */
int BpAddXml(BP_STATE *State, const wchar_t *Xml, BP_EVENT *Scratch);

/**
 * Finds the slowest matching item (applications and services first) for a startup entry. Name is the entry's display
 * name, FilePath its executable path (either may be NULL). Returns the item or NULL.
 */
const BP_ITEM *BpFindItem(const BP_STATE *State, const wchar_t *Name, const wchar_t *FilePath);

/** Impact from a measured start-up time: 1 low (< 1 s), 2 medium (< 3 s), 3 high. */
int BpRateImpact(uint32_t TotalMs);

#ifdef __cplusplus
}
#endif

#endif
