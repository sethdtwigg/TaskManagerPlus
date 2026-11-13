/*
 * TaskManagerPlus: interface exported by the ResourceTrends plugin so SessionReports can export a trends window.
 *
 * GetUsageTable(hours, &columns) returns a flat PPH_LIST of PPH_STRING cells (row major): the first `columns`
 * entries are the headers, then one row per process recorded in the last `hours` hours, sorted by average CPU.
 * Free with FreeTable. Main thread.
 */

#ifndef _RESOURCETRENDSINTF_H
#define _RESOURCETRENDSINTF_H

#define RESOURCE_TRENDS_PLUGIN_NAME L"TaskManagerPlus.ResourceTrends"
#define RESOURCE_TRENDS_INTERFACE_VERSION 1

typedef struct _RESOURCE_TRENDS_INTERFACE
{
    ULONG Version;
    PPH_LIST (NTAPI *GetUsageTable)(_In_ ULONG Hours, _Out_ PULONG Columns);
    VOID (NTAPI *FreeTable)(_In_ PPH_LIST Cells);
} RESOURCE_TRENDS_INTERFACE, *PRESOURCE_TRENDS_INTERFACE;

#endif
