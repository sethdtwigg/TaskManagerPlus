/*
 * TaskManagerPlus SessionReports: pure parsing for `--report <kind> <path>` (see cliparse.h).
 */

#include "cliparse.h"

static wchar_t SrFold(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
}

static int SrEqualNoCase(const wchar_t *A, const wchar_t *B)
{
    while (*A && *B)
    {
        if (SrFold(*A) != SrFold(*B))
            return 0;

        A++;
        B++;
    }

    return !*A && !*B;
}

int SrCliParseKind(const wchar_t *Text)
{
    if (!Text)
        return -1;

    if (SrEqualNoCase(Text, L"processes") || SrEqualNoCase(Text, L"process"))
        return 0;
    if (SrEqualNoCase(Text, L"startup"))
        return 1;
    if (SrEqualNoCase(Text, L"services"))
        return 2;
    if (SrEqualNoCase(Text, L"trends"))
        return 3;

    return -1;
}

int SrCliFormatFromPath(const wchar_t *Path)
{
    const wchar_t *dot = NULL;
    const wchar_t *p;

    if (!Path)
        return -1;

    for (p = Path; *p; p++)
    {
        if (*p == L'\\' || *p == L'/')
            dot = NULL; // a dot in a directory name is not an extension
        else if (*p == L'.')
            dot = p;
    }

    if (!dot)
        return -1;

    if (SrEqualNoCase(dot, L".csv"))
        return 0;
    if (SrEqualNoCase(dot, L".json"))
        return 1;
    if (SrEqualNoCase(dot, L".html") || SrEqualNoCase(dot, L".htm"))
        return 2;

    return -1;
}
