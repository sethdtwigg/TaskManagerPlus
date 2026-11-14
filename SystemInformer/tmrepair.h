/*
 * TaskManagerPlus: Task Manager replacement path repair, decision logic.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 * The "Make default" option stores an absolute exe path in the Image File Execution Options taskmgr.exe Debugger value.
 * When the portable folder is moved that path dangles. TmrDecide says whether to offer a repair, using only the values
 * passed in, so it can be tested with injected strings and never touches the registry.
 */

#ifndef _TMREPAIR_H_
#define _TMREPAIR_H_

#include <stddef.h>
#include <wchar.h>

typedef enum _TMR_DECISION
{
    TmrNothing,            // value is absent, empty or already points at this exe
    TmrNotOurs,            // belongs to another program: never touched
    TmrOtherCopyWorks,     // names a TaskManagerPlus exe that still exists elsewhere: leave it alone
    TmrSuppressed,         // the user chose "Don't ask again"
    TmrOffer               // dangling TaskManagerPlus path: offer to repair
} TMR_DECISION;

typedef int (*TMR_EXISTS_FN)(const wchar_t *Path, void *Context);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extracts the executable path from a Debugger value: a leading quoted part, or the whole text when unquoted
 * (an unquoted value with arguments is returned whole, which then simply fails the exists check).
 * Returns 1 and fills Path (NUL terminated) or 0 when empty / too long.
 */
int TmrExtractPath(const wchar_t *Debugger, wchar_t *Path, size_t PathCount);

/** Case-insensitive, slash-insensitive path equality. */
int TmrPathsEqual(const wchar_t *A, const wchar_t *B);

/** 1 when the file name part is one TaskManagerPlus ships under (TaskManagerPlus.exe, SystemInformer.exe) or equals OwnPath's name. */
int TmrIsOurExeName(const wchar_t *Path, const wchar_t *OwnPath);

TMR_DECISION TmrDecide(
    const wchar_t *Debugger,
    const wchar_t *OwnPath,
    int SuppressPrompt,
    TMR_EXISTS_FN Exists,
    void *Context,
    wchar_t *StalePath,
    size_t StalePathCount
    );

#ifdef __cplusplus
}
#endif

#endif
