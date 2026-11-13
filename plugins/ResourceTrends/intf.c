/*
 * TaskManagerPlus ResourceTrends: interface for SessionReports (a usage table over the last N hours).
 */

#include "resourcetrends.h"
#include <resourcetrendsintf.h>

#define TRS_INTF_COLUMNS 8

static PPH_STRING TrsIntfFormat(
    _In_ PCWSTR Format,
    ...
    )
{
    va_list argptr;
    PPH_STRING string;

    va_start(argptr, Format);
    string = PhFormatString_V(Format, argptr);
    va_end(argptr);
    return string;
}

static PPH_LIST NTAPI TrsIntfGetUsageTable(
    _In_ ULONG Hours,
    _Out_ PULONG Columns
    )
{
    static PCWSTR headers[TRS_INTF_COLUMNS] =
    {
        L"Process", L"PID", L"Average CPU %", L"Peak CPU %", L"Average private MB", L"Peak private MB", L"Peak working set MB", L"Samples"
    };
    PPH_LIST cells = PhCreateList(64);
    ULONG sampleCount = TrsStoreGetSampleCount();
    TR_AGGREGATE *rows = NULL;
    ULONG inRange = 0;
    ULONG count = 0;
    ULONG i;
    LARGE_INTEGER now;
    LONGLONG from;
    LONGLONG to;

    *Columns = TRS_INTF_COLUMNS;

    for (i = 0; i < TRS_INTF_COLUMNS; i++)
        PhAddItemList(cells, PhCreateString(headers[i]));

    if (!Hours)
        Hours = 1;

    PhQuerySystemTime(&now);
    to = now.QuadPart;

    if (sampleCount)
    {
        TR_SAMPLE *last = TrsStoreGetSample(sampleCount - 1);

        if (last && last->System.Time > to)
            to = last->System.Time;
    }

    from = to - (LONGLONG)Hours * TR_FILETIME_HOUR;

    if (sampleCount)
        count = TrAggregate(TrsStoreGetSamples(), sampleCount, from, to, 0, &rows, &inRange);

    for (i = 0; i < count; i++)
    {
        PPH_STRING name = TrsStoreGetName(rows[i].NameIndex);

        PhAddItemList(cells, name ? PhReferenceObject(name) : PhCreateString(L"?"));
        PhAddItemList(cells, TrsIntfFormat(L"%lu", rows[i].ProcessId));
        PhAddItemList(cells, TrsIntfFormat(L"%.2f", rows[i].AvgCpuPercent));
        PhAddItemList(cells, TrsIntfFormat(L"%.2f", rows[i].PeakCpuPercent));
        PhAddItemList(cells, TrsIntfFormat(L"%lu", rows[i].AvgPrivateKB / 1024));
        PhAddItemList(cells, TrsIntfFormat(L"%lu", rows[i].PeakPrivateKB / 1024));
        PhAddItemList(cells, TrsIntfFormat(L"%lu", rows[i].PeakWorkingSetKB / 1024));
        PhAddItemList(cells, TrsIntfFormat(L"%lu", rows[i].Samples));
    }

    if (rows)
        TrFreeAggregate(rows);

    return cells;
}

static VOID NTAPI TrsIntfFreeTable(
    _In_ PPH_LIST Cells
    )
{
    PhDereferenceObjects(Cells->Items, Cells->Count);
    PhDereferenceObject(Cells);
}

RESOURCE_TRENDS_INTERFACE TrsInterface =
{
    RESOURCE_TRENDS_INTERFACE_VERSION,
    TrsIntfGetUsageTable,
    TrsIntfFreeTable
};
