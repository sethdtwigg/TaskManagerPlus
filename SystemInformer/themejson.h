/*
 * TaskManagerPlus: theme palette import / export as JSON, validation.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own. Never allocates, never recurses
 * deeper than THJ_MAX_DEPTH, never reads past Length: any byte sequence gives a status, never a crash.
 *
 * File format (UTF-8 JSON):
 *   {
 *     "format": "TaskManagerPlus.Theme",
 *     "version": 1,
 *     "name": "My theme",                       // optional, printable, up to 47 characters
 *     "colors": {                               // all six required, each exactly once, "#RRGGBB"
 *       "foreground": "#1C1C1C", "background": "#2B2B2B", "background2": "#414141",
 *       "highlight": "#808080", "highlight2": "#8F8F8F", "text": "#FFFFFF"
 *     }
 *   }
 * Unknown extra keys are ignored. Rejected: anything that is not that shape, a version other than 1, a colour that is not
 * "#" plus six hex digits (number, short form, named colour, alpha), a duplicate key, a file over 64 KB, and a palette
 * whose text colour has less than 3:1 contrast against the background or the second background (unreadable).
 */

#ifndef _THEMEJSON_H_
#define _THEMEJSON_H_

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define THJ_MAX_BYTES 65536
#define THJ_MAX_DEPTH 16
#define THJ_COLOR_COUNT 6
#define THJ_NAME_MAX 48

enum
{
    ThjForeground,
    ThjBackground,
    ThjBackground2,
    ThjHighlight,
    ThjHighlight2,
    ThjText
};

typedef struct _THJ_THEME
{
    uint32_t Rgb[THJ_COLOR_COUNT]; // 0x00RRGGBB, indexed by the enum above
    char Name[THJ_NAME_MAX];       // UTF-8 ASCII subset, NUL terminated, may be empty
} THJ_THEME;

typedef enum _THJ_STATUS
{
    ThjOk,
    ThjErrTooLarge,
    ThjErrSyntax,
    ThjErrNotObject,
    ThjErrFormat,
    ThjErrVersion,
    ThjErrMissingColor,
    ThjErrDuplicate,
    ThjErrBadColor,
    ThjErrContrast
} THJ_STATUS;

#ifdef __cplusplus
extern "C" {
#endif

THJ_STATUS ThjParse(const char *Utf8, size_t Length, THJ_THEME *Theme);

/** Writes the JSON for Theme into Buffer (NUL terminated). Returns the length without the NUL, or 0 when it does not fit. */
size_t ThjWrite(const THJ_THEME *Theme, char *Buffer, size_t Capacity);

/** Parses "#RRGGBB" (exactly that). Returns 1 and sets *Rgb, or 0. */
int ThjParseColor(const char *Text, size_t Length, uint32_t *Rgb);

/** WCAG contrast ratio of two 0x00RRGGBB colours, 1.0 to 21.0. */
double ThjContrast(uint32_t A, uint32_t B);

/** Short plain-language reason for a status. */
const wchar_t *ThjStatusText(THJ_STATUS Status);

#ifdef __cplusplus
}
#endif

#endif
