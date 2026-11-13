/*
 * Unit tests for SessionReports (Phase 7 item 6): CSV, JSON and HTML escaping and structure.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../plugins/SessionReports/reportfmt.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static char *Text(RF_BUFFER *B)
{
    char *s = (char *)malloc(B->Length + 1);

    memcpy(s, B->Data, B->Length);
    s[B->Length] = 0;
    return s;
}

static int Has(const char *Hay, const char *Needle)
{
    return strstr(Hay, Needle) != NULL;
}

static void TestCsv(void)
{
    const wchar_t *headers[] = { L"Name", L"Path, with comma", L"Note" };
    const wchar_t *cells[] =
    {
        L"plain", L"C:\\Program Files\\a,b.exe", L"say \"hi\"",
        L"two\r\nlines", NULL, L"=cmd|' /C calc'!A0",
        L"+1+1", L"-2+3", L"@SUM(A1)",
        L"-", L"caf\u00e9 \u4e2d\u6587", L"\U0001F600"
    };
    RF_TABLE t = { L"T", 3, headers, 4, cells };
    RF_BUFFER b;
    char *s;

    memset(&b, 0, sizeof(b));
    CHECK(RfWriteCsv(&t, &b));
    s = Text(&b);
    CHECK(b.Length > 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF);
    CHECK(Has(s + 3, "Name,\"Path, with comma\",Note\r\n"));
    CHECK(Has(s, "plain,\"C:\\Program Files\\a,b.exe\",\"say \"\"hi\"\"\"\r\n"));
    CHECK(Has(s, "\"two\r\nlines\",,\"'=cmd|' /C calc'!A0\"\r\n")); // embedded newline quoted, NULL empty, formula defused
    CHECK(Has(s, "\"'+1+1\",\"'-2+3\",\"'@SUM(A1)\"\r\n"));
    CHECK(Has(s, "\"'-\",caf\xC3\xA9 \xE4\xB8\xAD\xE6\x96\x87,\xF0\x9F\x98\x80\r\n")); // UTF-8 incl. surrogate pair
    free(s);
    RfFree(&b);

    // empty table: header only
    t.RowCount = 0;
    memset(&b, 0, sizeof(b));
    CHECK(RfWriteCsv(&t, &b));
    CHECK(b.Length == 3 + strlen("Name,\"Path, with comma\",Note\r\n"));
    RfFree(&b);
}

static void TestJson(void)
{
    const wchar_t *headers[] = { L"Name", L"Cmd" };
    const wchar_t *cells[] =
    {
        L"a\"b\\c", L"line1\nline2\ttab\x01 end",
        L"<script>alert(1)</script> & co", L"\xD800 lone \u00e9",
    };
    RF_TABLE t = { L"Process \"list\"", 2, headers, 2, cells };
    RF_BUFFER b;
    char *s;

    memset(&b, 0, sizeof(b));
    CHECK(RfWriteJson(&t, L"2026-11-13T10:00:00", &b));
    s = Text(&b);
    CHECK(Has(s, "\"report\": \"Process \\\"list\\\"\""));
    CHECK(Has(s, "\"generated\": \"2026-11-13T10:00:00\""));
    CHECK(Has(s, "\"columns\": [\"Name\", \"Cmd\"]"));
    CHECK(Has(s, "\"Name\": \"a\\\"b\\\\c\""));
    CHECK(Has(s, "line1\\nline2\\ttab\\u0001 end"));
    CHECK(Has(s, "\\u003cscript\\u003ealert(1)\\u003c/script\\u003e \\u0026 co"));
    CHECK(!Has(s, "<script>"));
    CHECK(Has(s, "\xEF\xBF\xBD lone \xC3\xA9")); // lone surrogate -> U+FFFD
    CHECK(Has(s, "}\n  ]\n}"));
    free(s);
    RfFree(&b);

    t.RowCount = 0;
    memset(&b, 0, sizeof(b));
    CHECK(RfWriteJson(&t, L"x", &b));
    s = Text(&b);
    CHECK(Has(s, "\"rows\": []"));
    free(s);
    RfFree(&b);
}

static void TestHtml(void)
{
    const wchar_t *headers[] = { L"Name<th>", L"Path" };
    const wchar_t *cells[] =
    {
        L"<img src=x onerror=alert(1)>", L"it's \"quoted\" & <b>bold</b>\x01",
        L"normal", NULL
    };
    RF_TABLE t = { L"A & B <report>", 2, headers, 2, cells };
    RF_BUFFER b;
    char *s;

    memset(&b, 0, sizeof(b));
    CHECK(RfWriteHtml(&t, L"2026-11-13", &b));
    s = Text(&b);
    CHECK(Has(s, "<!DOCTYPE html>"));
    CHECK(Has(s, "<title>A &amp; B &lt;report&gt;</title>"));
    CHECK(Has(s, "<th>Name&lt;th&gt;</th>"));
    CHECK(Has(s, "<td>&lt;img src=x onerror=alert(1)&gt;</td>"));
    CHECK(Has(s, "<td>it&#39;s &quot;quoted&quot; &amp; &lt;b&gt;bold&lt;/b&gt; </td>"));
    CHECK(Has(s, "<td></td></tr>"));
    CHECK(Has(s, " 2 rows"));
    // self-contained: no script, no external references
    CHECK(!Has(s, "<script"));
    CHECK(!Has(s, "http://"));
    CHECK(!Has(s, "https://"));
    CHECK(!Has(s, "src="  "\"") && !Has(s, "<link"));
    CHECK(!Has(s, "<img"));
    CHECK(!Has(s, "@import") && !Has(s, "url("));
    CHECK(Has(s, "Content-Security-Policy"));
    CHECK(Has(s, "</body></html>"));
    free(s);
    RfFree(&b);
}

int main(void)
{
    TestCsv();
    TestJson();
    TestHtml();
    printf("reports: %d checks, %d failed\n", Checks, Failures);
    return Failures ? 1 : 0;
}
