/*
 * TaskManagerPlus SessionReports: pure parsing for the command line `--report <kind> <path>`.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 * Kind ids match SR_KIND and format ids match SR_FORMAT in sessionreports.h.
 *
 * Exit codes of the headless run:
 *   0 report written          2 bad arguments (unknown kind, unknown extension, no path)
 *   3 data source unavailable (its plugin is not loaded)      4 the file could not be written
 *   5 timed out waiting for the first process refresh
 */

#ifndef _SR_CLIPARSE_H_
#define _SR_CLIPARSE_H_

#include <wchar.h>

#define SR_CLI_EXIT_OK 0
#define SR_CLI_EXIT_BAD_ARGS 2
#define SR_CLI_EXIT_NO_SOURCE 3
#define SR_CLI_EXIT_WRITE_FAILED 4
#define SR_CLI_EXIT_TIMEOUT 5

#ifdef __cplusplus
extern "C" {
#endif

/** "processes" | "startup" | "services" | "trends" (any case; "process" is accepted too) to 0..3, or -1. */
int SrCliParseKind(const wchar_t *Text);

/** File extension of Path (.csv .json .html .htm, any case) to 0 = csv, 1 = json, 2 = html, or -1 when missing or unknown. */
int SrCliFormatFromPath(const wchar_t *Path);

#ifdef __cplusplus
}
#endif

#endif
