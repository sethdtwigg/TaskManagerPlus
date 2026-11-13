/*
 * TaskManagerPlus SessionReports: builds the tables (flat lists of PPH_STRING cells, headers first).
 */

#include "sessionreports.h"

PCWSTR SrGetKindName(
    _In_ SR_KIND Kind
    )
{
    static PCWSTR names[SrKindMaximum] = { L"processes", L"startup", L"services", L"trends" };

    return Kind < SrKindMaximum ? names[Kind] : L"report";
}

PCWSTR SrGetKindTitle(
    _In_ SR_KIND Kind
    )
{
    static PCWSTR titles[SrKindMaximum] = { L"Process list", L"Startup items", L"Services", L"Resource trends (process usage)" };

    return Kind < SrKindMaximum ? titles[Kind] : L"Report";
}

PCWSTR SrGetFormatExtension(
    _In_ SR_FORMAT Format
    )
{
    static PCWSTR names[SrFormatMaximum] = { L"csv", L"json", L"html" };

    return Format < SrFormatMaximum ? names[Format] : L"csv";
}

static VOID SrAddText(
    _In_ PPH_LIST Cells,
    _In_opt_ PPH_STRING Text
    )
{
    PhAddItemList(Cells, Text ? PhReferenceObject(Text) : PhReferenceEmptyString());
}

static VOID SrAddFormat(
    _In_ PPH_LIST Cells,
    _In_ PCWSTR Format,
    ...
    )
{
    va_list argptr;

    va_start(argptr, Format);
    PhAddItemList(Cells, PhFormatString_V(Format, argptr));
    va_end(argptr);
}

static VOID SrAddHeaders(
    _In_ PPH_LIST Cells,
    _In_reads_(Count) PCWSTR *Headers,
    _In_ ULONG Count
    )
{
    ULONG i;

    for (i = 0; i < Count; i++)
        PhAddItemList(Cells, PhCreateString(Headers[i]));
}

static PPH_STRING SrFormatLocalTime(
    _In_ PLARGE_INTEGER Time
    )
{
    SYSTEMTIME st;

    if (!Time->QuadPart)
        return PhReferenceEmptyString();

    PhLargeIntegerToLocalSystemTime(&st, Time);
    return PhFormatString(L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static PPH_LIST SrCollectProcesses(
    _Out_ PULONG Columns
    )
{
    static PCWSTR headers[] =
    {
        L"Name", L"PID", L"Parent PID", L"CPU %", L"Private MB", L"Working set MB", L"Threads", L"Handles",
        L"Priority", L"Started", L"User", L"Path", L"Command line"
    };
    PPH_LIST cells = PhCreateList(1024);
    PPH_PROCESS_ITEM *items;
    ULONG count;
    ULONG i;

    *Columns = RTL_NUMBER_OF(headers);
    SrAddHeaders(cells, headers, RTL_NUMBER_OF(headers));

    PhEnumProcessItems(&items, &count);

    for (i = 0; i < count; i++)
    {
        PPH_PROCESS_ITEM p = items[i];
        PPH_STRING path = NULL;
        PCPH_STRINGREF priority = NULL;

        if (!PH_IS_FAKE_PROCESS_ID(p->ProcessId))
        {
            if (p->FileName)
                path = PhGetFileName(p->FileName);

            if (p->PriorityClass)
                priority = PhGetProcessPriorityClassString(p->PriorityClass);

            SrAddText(cells, p->ProcessName);
            SrAddFormat(cells, L"%lu", HandleToUlong(p->ProcessId));
            SrAddFormat(cells, L"%lu", HandleToUlong(p->ParentProcessId));
            SrAddFormat(cells, L"%.1f", (double)p->CpuUsage * 100.0);
            SrAddFormat(cells, L"%.1f", (double)p->VmCounters.PagefileUsage / (1024.0 * 1024.0));
            SrAddFormat(cells, L"%.1f", (double)p->VmCounters.WorkingSetSize / (1024.0 * 1024.0));
            SrAddFormat(cells, L"%lu", p->NumberOfThreads);
            SrAddFormat(cells, L"%lu", p->NumberOfHandles);
            PhAddItemList(cells, priority ? PhCreateString2(priority) : PhReferenceEmptyString());
            PhAddItemList(cells, SrFormatLocalTime(&p->CreateTime));
            SrAddText(cells, p->UserName);
            SrAddText(cells, path);
            SrAddText(cells, p->CommandLine);
        }

        PhClearReference(&path);
        PhDereferenceObject(p);
    }

    PhFree(items);
    return cells;
}

static PPH_LIST SrCollectStartup(
    _Out_ PULONG Columns
    )
{
    static PCWSTR headers[] = { L"Name", L"Type", L"Status", L"Impact", L"Boot delay (ms)", L"Command", L"Path" };
    PSTARTUP_MANAGER_INTERFACE intf = PhGetPluginInterfaceZ(STARTUP_MANAGER_PLUGIN_NAME, STARTUP_MANAGER_INTERFACE_VERSION);
    PPH_LIST cells;
    PPH_LIST entries;
    ULONG i;

    *Columns = RTL_NUMBER_OF(headers);

    if (!intf)
        return NULL;

    cells = PhCreateList(256);
    SrAddHeaders(cells, headers, RTL_NUMBER_OF(headers));
    entries = intf->Enumerate();

    for (i = 0; i < entries->Count; i++)
    {
        PSTARTUP_ENTRY_INFO e = entries->Items[i];

        SrAddText(cells, e->Name);
        SrAddText(cells, e->Type);
        PhAddItemList(cells, PhCreateString(e->Enabled ? L"Enabled" : L"Disabled"));
        PhAddItemList(cells, PhCreateString(e->Impact ? e->Impact : L""));

        if (e->BootDelayMs)
            SrAddFormat(cells, L"%lu", e->BootDelayMs);
        else
            PhAddItemList(cells, PhReferenceEmptyString());

        SrAddText(cells, e->Command);
        SrAddText(cells, e->FileName);
    }

    intf->FreeEntries(entries);
    return cells;
}

static PCWSTR SrServiceState(
    _In_ ULONG State
    )
{
    switch (State)
    {
    case SERVICE_STOPPED: return L"Stopped";
    case SERVICE_START_PENDING: return L"Starting";
    case SERVICE_STOP_PENDING: return L"Stopping";
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_CONTINUE_PENDING: return L"Continuing";
    case SERVICE_PAUSE_PENDING: return L"Pausing";
    case SERVICE_PAUSED: return L"Paused";
    }

    return L"Unknown";
}

static PPH_LIST SrCollectServices(
    _Out_ PULONG Columns
    )
{
    static PCWSTR headers[] = { L"Name", L"Display name", L"State", L"Type", L"PID" };
    PPH_LIST cells = PhCreateList(512);
    LPENUM_SERVICE_STATUS_PROCESS services;
    ULONG count;
    ULONG i;

    *Columns = RTL_NUMBER_OF(headers);
    SrAddHeaders(cells, headers, RTL_NUMBER_OF(headers));

    if (!NT_SUCCESS(PhEnumServices(&services, &count)))
        return cells;

    for (i = 0; i < count; i++)
    {
        ULONG type = services[i].ServiceStatusProcess.dwServiceType;

        PhAddItemList(cells, PhCreateString(services[i].lpServiceName));
        PhAddItemList(cells, PhCreateString(services[i].lpDisplayName ? services[i].lpDisplayName : L""));
        PhAddItemList(cells, PhCreateString(SrServiceState(services[i].ServiceStatusProcess.dwCurrentState)));
        PhAddItemList(cells, PhCreateString((type & SERVICE_DRIVER) ? L"Driver" : (type & SERVICE_WIN32_SHARE_PROCESS) ? L"Shared process" : L"Own process"));

        if (services[i].ServiceStatusProcess.dwProcessId)
            SrAddFormat(cells, L"%lu", services[i].ServiceStatusProcess.dwProcessId);
        else
            PhAddItemList(cells, PhReferenceEmptyString());
    }

    PhFree(services);
    return cells;
}

static PPH_LIST SrCollectTrends(
    _In_ ULONG Hours,
    _Out_ PULONG Columns
    )
{
    PRESOURCE_TRENDS_INTERFACE intf = PhGetPluginInterfaceZ(RESOURCE_TRENDS_PLUGIN_NAME, RESOURCE_TRENDS_INTERFACE_VERSION);

    *Columns = 0;

    if (!intf)
        return NULL;

    return intf->GetUsageTable(Hours, Columns);
}

PPH_LIST SrCollect(
    _In_ SR_KIND Kind,
    _In_ ULONG TrendHours,
    _Out_ PULONG Columns
    )
{
    switch (Kind)
    {
    case SrKindProcesses:
        return SrCollectProcesses(Columns);
    case SrKindStartup:
        return SrCollectStartup(Columns);
    case SrKindServices:
        return SrCollectServices(Columns);
    case SrKindTrends:
        return SrCollectTrends(TrendHours, Columns);
    }

    *Columns = 0;
    return NULL;
}

VOID SrFreeTable(
    _In_ PPH_LIST Cells
    )
{
    PhDereferenceObjects(Cells->Items, Cells->Count);
    PhDereferenceObject(Cells);
}
