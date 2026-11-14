/*
 * TaskManagerPlus: Task Manager replacement path repair, decision logic (see tmrepair.h).
 */

#include "tmrepair.h"

static wchar_t TmrFold(wchar_t c)
{
    if (c == L'/')
        return L'\\';
    if (c >= L'A' && c <= L'Z')
        return (wchar_t)(c - L'A' + L'a');

    return c;
}

int TmrExtractPath(const wchar_t *Debugger, wchar_t *Path, size_t PathCount)
{
    size_t start = 0;
    size_t end;
    size_t length;

    if (!Debugger || !Path || !PathCount)
        return 0;

    while (Debugger[start] == L' ' || Debugger[start] == L'\t')
        start++;

    if (Debugger[start] == L'"')
    {
        start++;
        end = start;

        while (Debugger[end] && Debugger[end] != L'"')
            end++;
    }
    else
    {
        end = wcslen(Debugger);

        while (end > start && (Debugger[end - 1] == L' ' || Debugger[end - 1] == L'\t'))
            end--;
    }

    length = end - start;

    if (!length || length >= PathCount)
        return 0;

    wmemcpy(Path, Debugger + start, length);
    Path[length] = 0;
    return 1;
}

int TmrPathsEqual(const wchar_t *A, const wchar_t *B)
{
    if (!A || !B)
        return 0;

    while (*A && *B)
    {
        if (TmrFold(*A) != TmrFold(*B))
            return 0;

        A++;
        B++;
    }

    return !*A && !*B;
}

static const wchar_t *TmrBaseName(const wchar_t *Path)
{
    const wchar_t *base = Path;

    for (; *Path; Path++)
    {
        if (*Path == L'\\' || *Path == L'/')
            base = Path + 1;
    }

    return base;
}

int TmrIsOurExeName(const wchar_t *Path, const wchar_t *OwnPath)
{
    const wchar_t *name = TmrBaseName(Path);

    return TmrPathsEqual(name, L"TaskManagerPlus.exe") ||
        TmrPathsEqual(name, L"SystemInformer.exe") ||
        (OwnPath && TmrPathsEqual(name, TmrBaseName(OwnPath)));
}

TMR_DECISION TmrDecide(
    const wchar_t *Debugger,
    const wchar_t *OwnPath,
    int SuppressPrompt,
    TMR_EXISTS_FN Exists,
    void *Context,
    wchar_t *StalePath,
    size_t StalePathCount
    )
{
    wchar_t path[1024];

    if (StalePath && StalePathCount)
        StalePath[0] = 0;

    if (!Debugger || !OwnPath || !Debugger[0])
        return TmrNothing;

    if (!TmrExtractPath(Debugger, path, 1024))
        return TmrNothing;

    if (TmrPathsEqual(path, OwnPath))
        return TmrNothing;

    if (!TmrIsOurExeName(path, OwnPath))
        return TmrNotOurs;

    if (Exists && Exists(path, Context))
        return TmrOtherCopyWorks;

    if (SuppressPrompt)
        return TmrSuppressed;

    if (StalePath && StalePathCount && wcslen(path) < StalePathCount)
        wmemcpy(StalePath, path, wcslen(path) + 1);

    return TmrOffer;
}
