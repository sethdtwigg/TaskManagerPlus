/*
 * TaskManagerPlus SessionReports: CSV, JSON and HTML writers for a table of text cells.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 * Cells are UTF-16 (wchar_t on Windows) and are written as UTF-8.
 *   CSV:  RFC 4180 quoting (a cell containing , " CR or LF is quoted, quotes doubled), CRLF line ends, UTF-8 BOM so
 *         Excel picks the right code page. A cell that starts with = + - @ TAB or CR gets a leading ' so a spreadsheet
 *         never evaluates it as a formula (CSV injection).
 *   JSON: {"report","generated","columns":[...],"rows":[{column:text,...}]}. All values are strings. Quote, backslash,
 *         control characters, < > & are escaped; a lone surrogate becomes U+FFFD.
 *   HTML: one self-contained page: inline CSS only, no script, no external resource, a Content-Security-Policy meta that
 *         forbids everything but inline style. & < > " ' are escaped; control characters become spaces.
 */

#ifndef _RF_REPORTFMT_H_
#define _RF_REPORTFMT_H_

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

typedef struct _RF_TABLE
{
    const wchar_t *Title;
    uint32_t ColumnCount;
    const wchar_t *const *Headers; // ColumnCount entries
    uint32_t RowCount;
    const wchar_t *const *Cells; // RowCount * ColumnCount entries, row major; NULL entries are empty
} RF_TABLE;

typedef struct _RF_BUFFER
{
    uint8_t *Data; // malloc, not terminated
    size_t Length;
    size_t Capacity;
    int Failed; // set when an allocation failed; the output is then incomplete
} RF_BUFFER;

#ifdef __cplusplus
extern "C" {
#endif

/** Each writer appends to Buffer (zero-initialise it first). Returns 1 on success, 0 on allocation failure. */
int RfWriteCsv(const RF_TABLE *Table, RF_BUFFER *Buffer);
int RfWriteJson(const RF_TABLE *Table, const wchar_t *Generated, RF_BUFFER *Buffer);
int RfWriteHtml(const RF_TABLE *Table, const wchar_t *Generated, RF_BUFFER *Buffer);
void RfFree(RF_BUFFER *Buffer);

#ifdef __cplusplus
}
#endif

#endif
