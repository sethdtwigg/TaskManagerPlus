/*
 * TaskManagerPlus SessionReports: CSV, JSON and HTML writers. See reportfmt.h.
 */

#include "reportfmt.h"
#include <stdlib.h>
#include <string.h>

static int RfReserve(RF_BUFFER *B, size_t Extra)
{
    size_t need = B->Length + Extra;

    if (B->Failed)
        return 0;

    if (need > B->Capacity)
    {
        size_t cap = B->Capacity ? B->Capacity : 4096;
        uint8_t *p;

        while (cap < need)
            cap *= 2;

        p = (uint8_t *)realloc(B->Data, cap);

        if (!p)
        {
            B->Failed = 1;
            return 0;
        }

        B->Data = p;
        B->Capacity = cap;
    }

    return 1;
}

static void RfBytes(RF_BUFFER *B, const char *Text, size_t Length)
{
    if (RfReserve(B, Length))
    {
        memcpy(B->Data + B->Length, Text, Length);
        B->Length += Length;
    }
}

static void RfAscii(RF_BUFFER *B, const char *Text)
{
    RfBytes(B, Text, strlen(Text));
}

static void RfChar(RF_BUFFER *B, char C)
{
    RfBytes(B, &C, 1);
}

static void RfCodePoint(RF_BUFFER *B, uint32_t Cp)
{
    char t[4];

    if (Cp < 0x80)
    {
        t[0] = (char)Cp;
        RfBytes(B, t, 1);
    }
    else if (Cp < 0x800)
    {
        t[0] = (char)(0xC0 | (Cp >> 6));
        t[1] = (char)(0x80 | (Cp & 0x3F));
        RfBytes(B, t, 2);
    }
    else if (Cp < 0x10000)
    {
        t[0] = (char)(0xE0 | (Cp >> 12));
        t[1] = (char)(0x80 | ((Cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (Cp & 0x3F));
        RfBytes(B, t, 3);
    }
    else
    {
        t[0] = (char)(0xF0 | (Cp >> 18));
        t[1] = (char)(0x80 | ((Cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((Cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (Cp & 0x3F));
        RfBytes(B, t, 4);
    }
}

/** Reads one code point from UTF-16, advancing *P. A lone surrogate yields U+FFFD. */
static uint32_t RfNext(const wchar_t **P)
{
    uint32_t c = (uint16_t)**P;

    (*P)++;

    if (c >= 0xD800 && c <= 0xDBFF)
    {
        uint32_t d = (uint16_t)**P;

        if (d >= 0xDC00 && d <= 0xDFFF)
        {
            (*P)++;
            return 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
        }

        return 0xFFFD;
    }

    if (c >= 0xDC00 && c <= 0xDFFF)
        return 0xFFFD;

    return c;
}

// ---------------------------------------------------------------- CSV

static void RfCsvCell(RF_BUFFER *B, const wchar_t *Text)
{
    const wchar_t *p;
    int quote = 0;
    int prefix = 0;

    if (!Text)
        Text = L"";

    if (Text[0] == L'=' || Text[0] == L'+' || Text[0] == L'-' || Text[0] == L'@' || Text[0] == L'\t' || Text[0] == L'\r')
        prefix = 1;

    for (p = Text; *p; p++)
    {
        if (*p == L',' || *p == L'"' || *p == L'\r' || *p == L'\n')
            quote = 1;
    }

    if (prefix)
        quote = 1;

    if (quote)
        RfChar(B, '"');

    if (prefix)
        RfChar(B, '\'');

    p = Text;

    while (*p)
    {
        uint32_t cp = RfNext(&p);

        if (cp == '"')
            RfChar(B, '"');

        RfCodePoint(B, cp);
    }

    if (quote)
        RfChar(B, '"');
}

int RfWriteCsv(const RF_TABLE *T, RF_BUFFER *B)
{
    uint32_t r, c;

    RfBytes(B, "\xEF\xBB\xBF", 3);

    for (c = 0; c < T->ColumnCount; c++)
    {
        if (c)
            RfChar(B, ',');
        RfCsvCell(B, T->Headers[c]);
    }

    RfAscii(B, "\r\n");

    for (r = 0; r < T->RowCount; r++)
    {
        for (c = 0; c < T->ColumnCount; c++)
        {
            if (c)
                RfChar(B, ',');
            RfCsvCell(B, T->Cells[(size_t)r * T->ColumnCount + c]);
        }

        RfAscii(B, "\r\n");
    }

    return !B->Failed;
}

// ---------------------------------------------------------------- JSON

static void RfJsonString(RF_BUFFER *B, const wchar_t *Text)
{
    const wchar_t *p = Text ? Text : L"";

    RfChar(B, '"');

    while (*p)
    {
        uint32_t cp = RfNext(&p);

        switch (cp)
        {
        case '"':
            RfAscii(B, "\\\"");
            break;
        case '\\':
            RfAscii(B, "\\\\");
            break;
        case '\n':
            RfAscii(B, "\\n");
            break;
        case '\r':
            RfAscii(B, "\\r");
            break;
        case '\t':
            RfAscii(B, "\\t");
            break;
        default:
            if (cp < 0x20 || cp == '<' || cp == '>' || cp == '&' || cp == 0x2028 || cp == 0x2029 || cp == 0x7F)
            {
                char t[8];
                static const char hex[] = "0123456789abcdef";

                t[0] = '\\';
                t[1] = 'u';
                t[2] = hex[(cp >> 12) & 15];
                t[3] = hex[(cp >> 8) & 15];
                t[4] = hex[(cp >> 4) & 15];
                t[5] = hex[cp & 15];
                RfBytes(B, t, 6);
            }
            else
            {
                RfCodePoint(B, cp);
            }
            break;
        }
    }

    RfChar(B, '"');
}

int RfWriteJson(const RF_TABLE *T, const wchar_t *Generated, RF_BUFFER *B)
{
    uint32_t r, c;

    RfAscii(B, "{\n  \"report\": ");
    RfJsonString(B, T->Title);
    RfAscii(B, ",\n  \"generated\": ");
    RfJsonString(B, Generated);
    RfAscii(B, ",\n  \"columns\": [");

    for (c = 0; c < T->ColumnCount; c++)
    {
        if (c)
            RfAscii(B, ", ");
        RfJsonString(B, T->Headers[c]);
    }

    RfAscii(B, "],\n  \"rows\": [");

    for (r = 0; r < T->RowCount; r++)
    {
        RfAscii(B, r ? ",\n    {" : "\n    {");

        for (c = 0; c < T->ColumnCount; c++)
        {
            if (c)
                RfAscii(B, ", ");
            RfJsonString(B, T->Headers[c]);
            RfAscii(B, ": ");
            RfJsonString(B, T->Cells[(size_t)r * T->ColumnCount + c]);
        }

        RfChar(B, '}');
    }

    RfAscii(B, T->RowCount ? "\n  ]\n}\n" : "]\n}\n");
    return !B->Failed;
}

// ---------------------------------------------------------------- HTML

static void RfHtmlText(RF_BUFFER *B, const wchar_t *Text)
{
    const wchar_t *p = Text ? Text : L"";

    while (*p)
    {
        uint32_t cp = RfNext(&p);

        switch (cp)
        {
        case '&':
            RfAscii(B, "&amp;");
            break;
        case '<':
            RfAscii(B, "&lt;");
            break;
        case '>':
            RfAscii(B, "&gt;");
            break;
        case '"':
            RfAscii(B, "&quot;");
            break;
        case '\'':
            RfAscii(B, "&#39;");
            break;
        default:
            if (cp < 0x20 || cp == 0x7F)
                RfChar(B, ' ');
            else
                RfCodePoint(B, cp);
            break;
        }
    }
}

int RfWriteHtml(const RF_TABLE *T, const wchar_t *Generated, RF_BUFFER *B)
{
    uint32_t r, c;
    char count[32];

    RfAscii(B,
        "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
    RfHtmlText(B, T->Title);
    RfAscii(B,
        "</title>\n<style>\n"
        ":root{--bg:#fff;--fg:#1b1b1f;--dim:#5d5d66;--line:#d8d8de;--head:#f1f1f5;--alt:#f8f8fb}\n"
        "@media(prefers-color-scheme:dark){:root{--bg:#16161a;--fg:#e8e8ee;--dim:#a0a0ac;--line:#33333b;--head:#202026;--alt:#1b1b20}}\n"
        "body{margin:24px;background:var(--bg);color:var(--fg);font:14px/1.4 'Segoe UI',system-ui,sans-serif}\n"
        "h1{font-size:20px;margin:0 0 4px}p{color:var(--dim);margin:0 0 16px}\n"
        "table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:6px 10px;border-bottom:1px solid var(--line);vertical-align:top;word-break:break-word}\n"
        "th{background:var(--head);position:sticky;top:0}tr:nth-child(even) td{background:var(--alt)}\n"
        "</style></head><body>\n<h1>");
    RfHtmlText(B, T->Title);
    RfAscii(B, "</h1>\n<p>Generated ");
    RfHtmlText(B, Generated);
    RfAscii(B, " by TaskManagerPlus, ");
    count[0] = 0;
    {
        char tmp[16];
        int n = 0;
        uint32_t v = T->RowCount;

        do
        {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        } while (v);

        for (r = 0; (int)r < n; r++)
            count[r] = tmp[n - 1 - (int)r];

        count[n] = 0;
    }
    RfAscii(B, count);
    RfAscii(B, " rows</p>\n<table>\n<thead><tr>");

    for (c = 0; c < T->ColumnCount; c++)
    {
        RfAscii(B, "<th>");
        RfHtmlText(B, T->Headers[c]);
        RfAscii(B, "</th>");
    }

    RfAscii(B, "</tr></thead>\n<tbody>\n");

    for (r = 0; r < T->RowCount; r++)
    {
        RfAscii(B, "<tr>");

        for (c = 0; c < T->ColumnCount; c++)
        {
            RfAscii(B, "<td>");
            RfHtmlText(B, T->Cells[(size_t)r * T->ColumnCount + c]);
            RfAscii(B, "</td>");
        }

        RfAscii(B, "</tr>\n");
    }

    RfAscii(B, "</tbody>\n</table>\n</body></html>\n");
    return !B->Failed;
}

void RfFree(RF_BUFFER *B)
{
    free(B->Data);
    memset(B, 0, sizeof(*B));
}
