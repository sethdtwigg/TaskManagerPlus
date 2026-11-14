/*
 * TaskManagerPlus SessionReports plugin.
 *
 * Exports the current process list, startup list, services or a trends window (last N hours of ResourceTrends data)
 * to CSV, JSON or a self-contained HTML page, locally. Entry points: System > Export report, and the Ctrl+K palette.
 */

#ifndef _SESSIONREPORTS_H_
#define _SESSIONREPORTS_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"
#include <startupmanagerintf.h>
#include <resourcetrendsintf.h>
#include <commandsintf.h>
#include "reportfmt.h"

#define PLUGIN_NAME TMP_COMMANDS_REPORTS_PLUGIN_NAME
#define SETTING_NAME_LAST_DIRECTORY (PLUGIN_NAME L".LastDirectory")
#define SETTING_NAME_TREND_HOURS (PLUGIN_NAME L".TrendHours")
#define SETTING_NAME_TEST_DIRECTORY (PLUGIN_NAME L".TestDirectory") // TaskManagerPlus: tests write here without a dialog
#define EXPORT_MESSAGE_NAME L"TaskManagerPlus.SessionReports.Export" // wParam = SR_KIND, lParam = SR_FORMAT (tests)

typedef enum _SR_KIND
{
    SrKindProcesses,
    SrKindStartup,
    SrKindServices,
    SrKindTrends,
    SrKindMaximum
} SR_KIND;

typedef enum _SR_FORMAT
{
    SrFormatCsv,
    SrFormatJson,
    SrFormatHtml,
    SrFormatMaximum
} SR_FORMAT;

extern PPH_PLUGIN PluginInstance;
extern TMP_COMMANDS_INTERFACE SrCommandsInterface;

// collect.c

PCWSTR SrGetKindName(
    _In_ SR_KIND Kind
    );

PCWSTR SrGetKindTitle(
    _In_ SR_KIND Kind
    );

PCWSTR SrGetFormatExtension(
    _In_ SR_FORMAT Format
    );

/** Returns a flat list of PPH_STRING cells, headers first; free with SrFreeTable. NULL when the source is unavailable. */
PPH_LIST SrCollect(
    _In_ SR_KIND Kind,
    _In_ ULONG TrendHours,
    _Out_ PULONG Columns
    );

VOID SrFreeTable(
    _In_ PPH_LIST Cells
    );

// cli.c

/** Headless `--report <kind> <path>`: does nothing unless that is on the command line (see cli.c). */
VOID SrCliInitialize(
    VOID
    );

// export.c

NTSTATUS SrWriteReport(
    _In_ SR_KIND Kind,
    _In_ SR_FORMAT Format,
    _In_ PPH_LIST Cells,
    _In_ ULONG Columns,
    _In_ PPH_STRING FileName
    );

/** Collects, asks for a file name (the extension picks the format) and writes. */
VOID SrExportInteractive(
    _In_ HWND OwnerWindow,
    _In_ SR_KIND Kind,
    _In_ SR_FORMAT DefaultFormat
    );

/** No dialog: writes <TestDirectory>\report-<kind>.<ext>. Returns the status. */
NTSTATUS SrExportToDirectory(
    _In_ SR_KIND Kind,
    _In_ SR_FORMAT Format,
    _In_ PPH_STRING Directory
    );

#endif
