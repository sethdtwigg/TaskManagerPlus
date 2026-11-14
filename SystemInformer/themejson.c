/*
 * TaskManagerPlus: theme palette import / export as JSON (see themejson.h).
 */

#include <wchar.h>
#include "themejson.h"

static const uint32_t ThjLinear[256] = /* sRGB channel to linear light, millionths */
{
    0, 304, 607, 911, 1214, 1518, 1821, 2125,
    2428, 2732, 3035, 3347, 3677, 4025, 4391, 4777,
    5182, 5605, 6049, 6512, 6995, 7499, 8023, 8568,
    9134, 9721, 10330, 10960, 11612, 12286, 12983, 13702,
    14444, 15209, 15996, 16807, 17642, 18500, 19382, 20289,
    21219, 22174, 23153, 24158, 25187, 26241, 27321, 28426,
    29557, 30713, 31896, 33105, 34340, 35601, 36889, 38204,
    39546, 40915, 42311, 43735, 45186, 46665, 48172, 49707,
    51269, 52861, 54480, 56128, 57805, 59511, 61246, 63010,
    64803, 66626, 68478, 70360, 72272, 74214, 76185, 78187,
    80220, 82283, 84376, 86500, 88656, 90842, 93059, 95307,
    97587, 99899, 102242, 104616, 107023, 109462, 111932, 114435,
    116971, 119538, 122139, 124772, 127438, 130136, 132868, 135633,
    138432, 141263, 144128, 147027, 149960, 152926, 155926, 158961,
    162029, 165132, 168269, 171441, 174647, 177888, 181164, 184475,
    187821, 191202, 194618, 198069, 201556, 205079, 208637, 212231,
    215861, 219526, 223228, 226966, 230740, 234551, 238398, 242281,
    246201, 250158, 254152, 258183, 262251, 266356, 270498, 274677,
    278894, 283149, 287441, 291771, 296138, 300544, 304987, 309469,
    313989, 318547, 323143, 327778, 332452, 337164, 341914, 346704,
    351533, 356400, 361307, 366253, 371238, 376262, 381326, 386429,
    391572, 396755, 401978, 407240, 412543, 417885, 423268, 428690,
    434154, 439657, 445201, 450786, 456411, 462077, 467784, 473531,
    479320, 485150, 491021, 496933, 502886, 508881, 514918, 520996,
    527115, 533276, 539479, 545724, 552011, 558340, 564712, 571125,
    577580, 584078, 590619, 597202, 603827, 610496, 617207, 623960,
    630757, 637597, 644480, 651406, 658375, 665387, 672443, 679542,
    686685, 693872, 701102, 708376, 715694, 723055, 730461, 737910,
    745404, 752942, 760525, 768151, 775822, 783538, 791298, 799103,
    806952, 814847, 822786, 830770, 838799, 846873, 854993, 863157,
    871367, 879622, 887923, 896269, 904661, 913099, 921582, 930111,
    938686, 947307, 955973, 964686, 973445, 982251, 991102, 1000000,
};
static const char *ThjColorNames[THJ_COLOR_COUNT] = { "foreground", "background", "background2", "highlight", "highlight2", "text" };

typedef struct _THJ_PARSER
{
    const char *Cursor;
    const char *End;
} THJ_PARSER;

static void ThjSkipSpace(THJ_PARSER *P)
{
    while (P->Cursor < P->End && (*P->Cursor == ' ' || *P->Cursor == '\t' || *P->Cursor == '\r' || *P->Cursor == '\n'))
        P->Cursor++;
}

static int ThjHexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

/**
 * Parses a JSON string at the cursor (which must be on the opening quote) into Out as an ASCII subset: escapes are
 * decoded, anything that is not printable ASCII becomes '?'. Out may be NULL; *Truncated is set when it did not fit.
 * Returns 1 on a well formed string.
 */
static int ThjParseString(THJ_PARSER *P, char *Out, size_t Capacity, size_t *Length)
{
    size_t length = 0;

    if (P->Cursor >= P->End || *P->Cursor != '"')
        return 0;

    P->Cursor++;

    for (;;)
    {
        unsigned char c;
        char decoded;

        if (P->Cursor >= P->End)
            return 0;

        c = (unsigned char)*P->Cursor++;

        if (c == '"')
            break;
        if (c < 0x20)
            return 0; // raw control characters are not allowed in JSON strings

        if (c == '\\')
        {
            if (P->Cursor >= P->End)
                return 0;

            c = (unsigned char)*P->Cursor++;

            switch (c)
            {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': case 'f': case 'n': case 'r': case 't': decoded = ' '; break;
            case 'u':
                {
                    int i;
                    unsigned value = 0;

                    for (i = 0; i < 4; i++)
                    {
                        int h;

                        if (P->Cursor >= P->End || (h = ThjHexValue(*P->Cursor)) < 0)
                            return 0;

                        value = value * 16 + (unsigned)h;
                        P->Cursor++;
                    }

                    decoded = (value >= 0x20 && value < 0x7F) ? (char)value : '?';
                }
                break;
            default:
                return 0;
            }
        }
        else
        {
            decoded = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }

        if (Out && length + 1 < Capacity)
            Out[length] = decoded;

        length++;
    }

    if (Out && Capacity)
        Out[length < Capacity ? length : Capacity - 1] = 0;

    if (Length)
        *Length = length;

    return 1;
}

static int ThjSkipValue(THJ_PARSER *P, int Depth);

static int ThjSkipContainer(THJ_PARSER *P, int Depth, char Close, int IsObject)
{
    P->Cursor++; // opening bracket
    ThjSkipSpace(P);

    if (P->Cursor < P->End && *P->Cursor == Close)
    {
        P->Cursor++;
        return 1;
    }

    for (;;)
    {
        ThjSkipSpace(P);

        if (IsObject)
        {
            if (!ThjParseString(P, NULL, 0, NULL))
                return 0;

            ThjSkipSpace(P);

            if (P->Cursor >= P->End || *P->Cursor != ':')
                return 0;

            P->Cursor++;
        }

        if (!ThjSkipValue(P, Depth + 1))
            return 0;

        ThjSkipSpace(P);

        if (P->Cursor >= P->End)
            return 0;

        if (*P->Cursor == ',')
        {
            P->Cursor++;
            continue;
        }

        if (*P->Cursor == Close)
        {
            P->Cursor++;
            return 1;
        }

        return 0;
    }
}

static int ThjSkipNumber(THJ_PARSER *P)
{
    const char *start = P->Cursor;
    int digits = 0;

    if (P->Cursor < P->End && *P->Cursor == '-')
        P->Cursor++;

    if (P->Cursor >= P->End || *P->Cursor < '0' || *P->Cursor > '9')
        return 0;

    if (*P->Cursor == '0')
    {
        P->Cursor++;
        digits = 1;
    }
    else
    {
        while (P->Cursor < P->End && *P->Cursor >= '0' && *P->Cursor <= '9')
        {
            P->Cursor++;
            digits++;
        }
    }

    if (P->Cursor < P->End && *P->Cursor == '.')
    {
        int fraction = 0;

        P->Cursor++;

        while (P->Cursor < P->End && *P->Cursor >= '0' && *P->Cursor <= '9')
        {
            P->Cursor++;
            fraction++;
        }

        if (!fraction)
            return 0;
    }

    if (P->Cursor < P->End && (*P->Cursor == 'e' || *P->Cursor == 'E'))
    {
        int exponent = 0;

        P->Cursor++;

        if (P->Cursor < P->End && (*P->Cursor == '+' || *P->Cursor == '-'))
            P->Cursor++;

        while (P->Cursor < P->End && *P->Cursor >= '0' && *P->Cursor <= '9')
        {
            P->Cursor++;
            exponent++;
        }

        if (!exponent)
            return 0;
    }

    return digits > 0 && P->Cursor > start;
}

static int ThjMatchLiteral(THJ_PARSER *P, const char *Literal)
{
    const char *p = P->Cursor;

    while (*Literal)
    {
        if (p >= P->End || *p != *Literal)
            return 0;

        p++;
        Literal++;
    }

    P->Cursor = p;
    return 1;
}

static int ThjSkipValue(THJ_PARSER *P, int Depth)
{
    if (Depth > THJ_MAX_DEPTH)
        return 0;

    ThjSkipSpace(P);

    if (P->Cursor >= P->End)
        return 0;

    switch (*P->Cursor)
    {
    case '"': return ThjParseString(P, NULL, 0, NULL);
    case '{': return ThjSkipContainer(P, Depth, '}', 1);
    case '[': return ThjSkipContainer(P, Depth, ']', 0);
    case 't': return ThjMatchLiteral(P, "true");
    case 'f': return ThjMatchLiteral(P, "false");
    case 'n': return ThjMatchLiteral(P, "null");
    }

    return ThjSkipNumber(P);
}

int ThjParseColor(const char *Text, size_t Length, uint32_t *Rgb)
{
    uint32_t value = 0;
    size_t i;

    if (!Text || Length != 7 || Text[0] != '#')
        return 0;

    for (i = 1; i < 7; i++)
    {
        int h = ThjHexValue(Text[i]);

        if (h < 0)
            return 0;

        value = (value << 4) | (uint32_t)h;
    }

    if (Rgb)
        *Rgb = value;

    return 1;
}

static uint32_t ThjLuminance(uint32_t Rgb)
{
    // 0.2126 R + 0.7152 G + 0.0722 B, in millionths
    return (uint32_t)(((uint64_t)ThjLinear[(Rgb >> 16) & 0xFF] * 2126u + (uint64_t)ThjLinear[(Rgb >> 8) & 0xFF] * 7152u + (uint64_t)ThjLinear[Rgb & 0xFF] * 722u) / 10000u);
}

double ThjContrast(uint32_t A, uint32_t B)
{
    double la = (double)ThjLuminance(A) / 1000000.0 + 0.05;
    double lb = (double)ThjLuminance(B) / 1000000.0 + 0.05;

    return la > lb ? la / lb : lb / la;
}

static THJ_STATUS ThjParseColors(THJ_PARSER *P, THJ_THEME *Theme, int *Seen)
{
    char key[32];
    char value[16];
    size_t length;
    int depthGuard = 0;

    ThjSkipSpace(P);

    if (P->Cursor >= P->End || *P->Cursor != '{')
        return ThjErrMissingColor;

    P->Cursor++;
    ThjSkipSpace(P);

    if (P->Cursor < P->End && *P->Cursor == '}')
    {
        P->Cursor++;
        return ThjOk; // missing colours are reported by the caller
    }

    for (;;)
    {
        int index = -1;
        int i;

        if (++depthGuard > 64)
            return ThjErrSyntax;

        ThjSkipSpace(P);

        if (!ThjParseString(P, key, sizeof(key), &length))
            return ThjErrSyntax;

        ThjSkipSpace(P);

        if (P->Cursor >= P->End || *P->Cursor != ':')
            return ThjErrSyntax;

        P->Cursor++;
        ThjSkipSpace(P);

        for (i = 0; i < THJ_COLOR_COUNT; i++)
        {
            size_t n = 0;

            while (ThjColorNames[i][n] && n < sizeof(key) && key[n] == ThjColorNames[i][n])
                n++;

            if (!ThjColorNames[i][n] && length == n)
            {
                index = i;
                break;
            }
        }

        if (index >= 0)
        {
            if (Seen[index])
                return ThjErrDuplicate;

            if (P->Cursor >= P->End || *P->Cursor != '"')
                return ThjErrBadColor;

            if (!ThjParseString(P, value, sizeof(value), &length))
                return ThjErrSyntax;

            if (length >= sizeof(value) || !ThjParseColor(value, length, &Theme->Rgb[index]))
                return ThjErrBadColor;

            Seen[index] = 1;
        }
        else if (!ThjSkipValue(P, 3))
        {
            return ThjErrSyntax;
        }

        ThjSkipSpace(P);

        if (P->Cursor >= P->End)
            return ThjErrSyntax;

        if (*P->Cursor == ',')
        {
            P->Cursor++;
            continue;
        }

        if (*P->Cursor == '}')
        {
            P->Cursor++;
            return ThjOk;
        }

        return ThjErrSyntax;
    }
}

static int ThjKeyIs(const char *Key, size_t Length, const char *Name)
{
    size_t n = 0;

    while (Name[n] && n < Length && Key[n] == Name[n])
        n++;

    return !Name[n] && n == Length;
}

THJ_STATUS ThjParse(const char *Utf8, size_t Length, THJ_THEME *Theme)
{
    THJ_PARSER parser;
    THJ_PARSER *P = &parser;
    THJ_STATUS status;
    int seenColors[THJ_COLOR_COUNT] = { 0 };
    int seenFormat = 0;
    int seenVersion = 0;
    int seenColorsObject = 0;
    int seenName = 0;
    char key[32];
    char text[64];
    size_t length;
    int i;
    int keys = 0;

    if (!Utf8 || !Theme)
        return ThjErrSyntax;

    for (i = 0; i < THJ_COLOR_COUNT; i++)
        Theme->Rgb[i] = 0;

    Theme->Name[0] = 0;

    if (Length > THJ_MAX_BYTES)
        return ThjErrTooLarge;

    parser.Cursor = Utf8;
    parser.End = Utf8 + Length;

    if (Length >= 3 && (unsigned char)Utf8[0] == 0xEF && (unsigned char)Utf8[1] == 0xBB && (unsigned char)Utf8[2] == 0xBF)
        parser.Cursor += 3; // UTF-8 BOM

    ThjSkipSpace(P);

    if (P->Cursor >= P->End)
        return ThjErrSyntax;

    if (*P->Cursor != '{')
        return (*P->Cursor == '[' || *P->Cursor == '"' || (*P->Cursor >= '0' && *P->Cursor <= '9') || *P->Cursor == '-' ||
            *P->Cursor == 't' || *P->Cursor == 'f' || *P->Cursor == 'n') ? ThjErrNotObject : ThjErrSyntax;

    P->Cursor++;
    ThjSkipSpace(P);

    if (P->Cursor < P->End && *P->Cursor == '}')
        return ThjErrFormat; // empty object

    for (;;)
    {
        if (++keys > 256)
            return ThjErrSyntax;

        ThjSkipSpace(P);

        if (!ThjParseString(P, key, sizeof(key), &length))
            return ThjErrSyntax;

        ThjSkipSpace(P);

        if (P->Cursor >= P->End || *P->Cursor != ':')
            return ThjErrSyntax;

        P->Cursor++;
        ThjSkipSpace(P);

        if (ThjKeyIs(key, length, "format"))
        {
            if (seenFormat)
                return ThjErrDuplicate;

            if (P->Cursor >= P->End || *P->Cursor != '"')
                return ThjErrFormat;

            if (!ThjParseString(P, text, sizeof(text), &length))
                return ThjErrSyntax;

            if (!ThjKeyIs(text, length, "TaskManagerPlus.Theme"))
                return ThjErrFormat;

            seenFormat = 1;
        }
        else if (ThjKeyIs(key, length, "version"))
        {
            const char *start;

            if (seenVersion)
                return ThjErrDuplicate;

            start = P->Cursor;

            if (!ThjSkipNumber(P))
                return (P->Cursor < P->End && *start != '"' && *start != '{' && *start != '[') ? ThjErrSyntax : ThjErrVersion;

            if (P->Cursor - start != 1 || *start != '1')
                return ThjErrVersion;

            seenVersion = 1;
        }
        else if (ThjKeyIs(key, length, "name"))
        {
            if (seenName)
                return ThjErrDuplicate;

            if (P->Cursor < P->End && *P->Cursor == '"')
            {
                if (!ThjParseString(P, Theme->Name, THJ_NAME_MAX, &length))
                    return ThjErrSyntax;
            }
            else if (!ThjSkipValue(P, 1))
            {
                return ThjErrSyntax;
            }

            seenName = 1;
        }
        else if (ThjKeyIs(key, length, "colors"))
        {
            if (seenColorsObject)
                return ThjErrDuplicate;

            if (P->Cursor >= P->End || *P->Cursor != '{')
                return ThjErrMissingColor;

            status = ThjParseColors(P, Theme, seenColors);

            if (status != ThjOk)
                return status;

            seenColorsObject = 1;
        }
        else if (!ThjSkipValue(P, 1))
        {
            return ThjErrSyntax;
        }

        ThjSkipSpace(P);

        if (P->Cursor >= P->End)
            return ThjErrSyntax;

        if (*P->Cursor == ',')
        {
            P->Cursor++;
            continue;
        }

        if (*P->Cursor == '}')
        {
            P->Cursor++;
            break;
        }

        return ThjErrSyntax;
    }

    ThjSkipSpace(P);

    if (P->Cursor != P->End)
        return ThjErrSyntax; // trailing garbage

    if (!seenFormat)
        return ThjErrFormat;

    if (!seenVersion)
        return ThjErrVersion;

    if (!seenColorsObject)
        return ThjErrMissingColor;

    for (i = 0; i < THJ_COLOR_COUNT; i++)
    {
        if (!seenColors[i])
            return ThjErrMissingColor;
    }

    if (ThjContrast(Theme->Rgb[ThjText], Theme->Rgb[ThjBackground]) < 3.0 ||
        ThjContrast(Theme->Rgb[ThjText], Theme->Rgb[ThjBackground2]) < 3.0)
        return ThjErrContrast;

    return ThjOk;
}

static size_t ThjAppend(char *Buffer, size_t Capacity, size_t Used, const char *Text)
{
    while (*Text)
    {
        if (Used + 1 >= Capacity)
            return (size_t)-1;

        Buffer[Used++] = *Text++;
    }

    Buffer[Used] = 0;
    return Used;
}

size_t ThjWrite(const THJ_THEME *Theme, char *Buffer, size_t Capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    char name[THJ_NAME_MAX * 2];
    size_t n = 0;
    int i;

    if (!Theme || !Buffer || !Capacity)
        return 0;

    Buffer[0] = 0;

    // The name is escaped for JSON; only printable ASCII is ever kept.
    for (i = 0; i < THJ_NAME_MAX && Theme->Name[i] && n + 2 < sizeof(name); i++)
    {
        char c = Theme->Name[i];

        if (c < 0x20 || c >= 0x7F)
            c = '?';

        if (c == '"' || c == '\\')
            name[n++] = '\\';

        name[n++] = c;
    }

    name[n] = 0;

    if ((used = ThjAppend(Buffer, Capacity, used, "{\r\n  \"format\": \"TaskManagerPlus.Theme\",\r\n  \"version\": 1,\r\n  \"name\": \"")) == (size_t)-1 ||
        (used = ThjAppend(Buffer, Capacity, used, name)) == (size_t)-1 ||
        (used = ThjAppend(Buffer, Capacity, used, "\",\r\n  \"colors\": {\r\n")) == (size_t)-1)
        return 0;

    for (i = 0; i < THJ_COLOR_COUNT; i++)
    {
        char line[64];
        const char *label = ThjColorNames[i];
        size_t l = 0;
        uint32_t rgb = Theme->Rgb[i];

        line[l++] = ' ';
        line[l++] = ' ';
        line[l++] = ' ';
        line[l++] = ' ';
        line[l++] = '"';

        while (*label)
            line[l++] = *label++;

        line[l++] = '"';
        line[l++] = ':';
        line[l++] = ' ';
        line[l++] = '"';
        line[l++] = '#';
        line[l++] = hex[(rgb >> 20) & 0xF];
        line[l++] = hex[(rgb >> 16) & 0xF];
        line[l++] = hex[(rgb >> 12) & 0xF];
        line[l++] = hex[(rgb >> 8) & 0xF];
        line[l++] = hex[(rgb >> 4) & 0xF];
        line[l++] = hex[rgb & 0xF];
        line[l++] = '"';

        if (i + 1 < THJ_COLOR_COUNT)
            line[l++] = ',';

        line[l++] = '\r';
        line[l++] = '\n';
        line[l] = 0;

        if ((used = ThjAppend(Buffer, Capacity, used, line)) == (size_t)-1)
            return 0;
    }

    if ((used = ThjAppend(Buffer, Capacity, used, "  }\r\n}\r\n")) == (size_t)-1)
        return 0;

    return used;
}

const wchar_t *ThjStatusText(THJ_STATUS Status)
{
    switch (Status)
    {
    case ThjOk: return L"The theme file is valid.";
    case ThjErrTooLarge: return L"The file is too large to be a theme file (over 64 KB).";
    case ThjErrSyntax: return L"The file is not valid JSON.";
    case ThjErrNotObject: return L"The file does not contain a theme object.";
    case ThjErrFormat: return L"This is not a TaskManagerPlus theme file (the \"format\" entry is missing or different).";
    case ThjErrVersion: return L"This theme file has a version this program does not understand (expected 1).";
    case ThjErrMissingColor: return L"The \"colors\" section is missing or does not have all six colors.";
    case ThjErrDuplicate: return L"An entry appears more than once.";
    case ThjErrBadColor: return L"A color is not written as #RRGGBB (six hexadecimal digits).";
    case ThjErrContrast: return L"The text color is too close to the background colors; the theme would be unreadable.";
    }

    return L"The theme file could not be read.";
}
