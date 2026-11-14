/*
 * Unit tests for theme JSON import/export (TaskManagerPlus SystemInformer\themejson.c): a valid document, round trip,
 * every error status, truncation at every length, byte mutation of every position, deterministic random garbage and
 * hostile nesting. The parser must give a status for every input and never crash or read out of bounds.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../SystemInformer/themejson.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *Valid =
    "{\n"
    "  \"format\": \"TaskManagerPlus.Theme\",\n"
    "  \"version\": 1,\n"
    "  \"name\": \"Test theme\",\n"
    "  \"colors\": {\n"
    "    \"foreground\": \"#1C1C1C\", \"background\": \"#2B2B2B\", \"background2\": \"#414141\",\n"
    "    \"highlight\": \"#808080\", \"highlight2\": \"#8f8f8f\", \"text\": \"#FFFFFF\"\n"
    "  }\n"
    "}\n";

static THJ_STATUS Parse(const char *Text)
{
    THJ_THEME theme;

    return ThjParse(Text, strlen(Text), &theme);
}

/** Valid document with one substitution, to build the error cases. */
static THJ_STATUS ParseWith(const char *Find, const char *Replace)
{
    char buffer[2048];
    const char *at = strstr(Valid, Find);
    size_t head;

    CHECK(at != NULL);

    if (!at)
        return ThjErrSyntax;

    head = (size_t)(at - Valid);
    memcpy(buffer, Valid, head);
    strcpy(buffer + head, Replace);
    strcat(buffer, at + strlen(Find));
    return Parse(buffer);
}

static void TestValid(void)
{
    THJ_THEME theme;
    char bom[2048];

    CHECK(ThjParse(Valid, strlen(Valid), &theme) == ThjOk);
    CHECK(theme.Rgb[ThjForeground] == 0x1C1C1Cu);
    CHECK(theme.Rgb[ThjBackground] == 0x2B2B2Bu);
    CHECK(theme.Rgb[ThjBackground2] == 0x414141u);
    CHECK(theme.Rgb[ThjHighlight] == 0x808080u);
    CHECK(theme.Rgb[ThjHighlight2] == 0x8F8F8Fu); // lower case hex accepted
    CHECK(theme.Rgb[ThjText] == 0xFFFFFFu);
    CHECK(!strcmp(theme.Name, "Test theme"));

    // UTF-8 BOM, CRLF, no whitespace at all, unknown keys of every JSON type are ignored
    strcpy(bom, "\xEF\xBB\xBF");
    strcat(bom, Valid);
    CHECK(Parse(bom) == ThjOk);
    CHECK(Parse("{\"format\":\"TaskManagerPlus.Theme\",\"version\":1,\"colors\":{\"foreground\":\"#000000\",\"background\":\"#000000\","
                "\"background2\":\"#101010\",\"highlight\":\"#333333\",\"highlight2\":\"#444444\",\"text\":\"#FFFFFF\"}}") == ThjOk);
    CHECK(Parse("{\"extra\":[1,2,{\"a\":null}],\"format\":\"TaskManagerPlus.Theme\",\"n\":-1.5e+3,\"version\":1,\"t\":true,\"f\":false,"
                "\"colors\":{\"other\":{\"x\":1},\"foreground\":\"#000000\",\"background\":\"#000000\",\"background2\":\"#101010\","
                "\"highlight\":\"#333333\",\"highlight2\":\"#444444\",\"text\":\"#FFFFFF\"}}") == ThjOk);
}

static void TestWriteRoundTrip(void)
{
    THJ_THEME in;
    THJ_THEME out;
    char buffer[1024];
    char small[32];
    size_t length;
    int i;

    memset(&in, 0, sizeof(in));
    in.Rgb[ThjForeground] = 0x123456;
    in.Rgb[ThjBackground] = 0x0A0B0C;
    in.Rgb[ThjBackground2] = 0x202122;
    in.Rgb[ThjHighlight] = 0xABCDEF;
    in.Rgb[ThjHighlight2] = 0x010203;
    in.Rgb[ThjText] = 0xF0F0F0;
    strcpy(in.Name, "Quote \" and \\ slash");

    length = ThjWrite(&in, buffer, sizeof(buffer));
    CHECK(length > 0 && length == strlen(buffer));
    CHECK(ThjParse(buffer, length, &out) == ThjOk);

    for (i = 0; i < THJ_COLOR_COUNT; i++)
        CHECK(out.Rgb[i] == in.Rgb[i]);

    CHECK(!strcmp(out.Name, in.Name));

    // a buffer that is too small gives 0, never an overrun, at every size
    for (length = 1; length < 300; length++)
    {
        char tmp[300];

        memset(tmp, 0x5A, sizeof(tmp));
        CHECK(ThjWrite(&in, tmp, length) == 0 || strlen(tmp) < length);
        CHECK(tmp[length] == 0x5A); // nothing written past the stated capacity
    }

    CHECK(ThjWrite(&in, small, sizeof(small)) == 0);
    CHECK(ThjWrite(NULL, buffer, sizeof(buffer)) == 0);
    CHECK(ThjWrite(&in, NULL, 10) == 0);
    CHECK(ThjWrite(&in, buffer, 0) == 0);

    // a name with control characters / non-ASCII is written safely and still parses
    memset(&in.Name, 0, sizeof(in.Name));
    in.Name[0] = 'a'; in.Name[1] = '\n'; { unsigned char hi = 0xC3; in.Name[2] = (char)hi; } in.Name[3] = 'b';
    length = ThjWrite(&in, buffer, sizeof(buffer));
    CHECK(length > 0 && ThjParse(buffer, length, &out) == ThjOk && !strcmp(out.Name, "a??b"));
}

static void TestErrors(void)
{
    char big[THJ_MAX_BYTES + 100];
    char deep[4096];
    THJ_THEME theme;
    size_t i;

    CHECK(Parse("") == ThjErrSyntax);
    CHECK(Parse("   ") == ThjErrSyntax);
    CHECK(Parse("[]") == ThjErrNotObject);
    CHECK(Parse("\"str\"") == ThjErrNotObject);
    CHECK(Parse("42") == ThjErrNotObject);
    CHECK(Parse("null") == ThjErrNotObject);
    CHECK(Parse("{}") == ThjErrFormat);
    CHECK(Parse("{") == ThjErrSyntax);
    CHECK(Parse("}") == ThjErrSyntax);
    CHECK(Parse("garbage") == ThjErrSyntax);
    CHECK(ThjParse(NULL, 5, &theme) == ThjErrSyntax);
    CHECK(ThjParse("{}", 2, NULL) == ThjErrSyntax);

    CHECK(ParseWith("\"TaskManagerPlus.Theme\"", "\"Other.Theme\"") == ThjErrFormat);
    CHECK(ParseWith("\"TaskManagerPlus.Theme\"", "7") == ThjErrFormat);
    CHECK(ParseWith("\"format\": \"TaskManagerPlus.Theme\",", "") == ThjErrFormat);
    CHECK(ParseWith("\"version\": 1,", "") == ThjErrVersion);
    CHECK(ParseWith("\"version\": 1", "\"version\": 2") == ThjErrVersion);
    CHECK(ParseWith("\"version\": 1", "\"version\": 0") == ThjErrVersion);
    CHECK(ParseWith("\"version\": 1", "\"version\": 10") == ThjErrVersion);
    CHECK(ParseWith("\"version\": 1", "\"version\": \"1\"") == ThjErrVersion);
    CHECK(ParseWith("\"version\": 1", "\"version\": 1.0") == ThjErrVersion);
    CHECK(ParseWith("\"version\": 1", "\"version\": x") == ThjErrSyntax);

    // colours
    CHECK(ParseWith("\"#FFFFFF\"", "\"#FFFFF\"") == ThjErrBadColor);       // short
    CHECK(ParseWith("\"#FFFFFF\"", "\"#FFFFFFF\"") == ThjErrBadColor);     // long
    CHECK(ParseWith("\"#FFFFFF\"", "\"#FFFFFFFF\"") == ThjErrBadColor);    // alpha
    CHECK(ParseWith("\"#FFFFFF\"", "\"FFFFFF\"") == ThjErrBadColor);       // no hash
    CHECK(ParseWith("\"#FFFFFF\"", "\"#GGGGGG\"") == ThjErrBadColor);      // not hex
    CHECK(ParseWith("\"#FFFFFF\"", "\"#FFF\"") == ThjErrBadColor);         // css short form
    CHECK(ParseWith("\"#FFFFFF\"", "\"white\"") == ThjErrBadColor);        // named
    CHECK(ParseWith("\"#FFFFFF\"", "\"\"") == ThjErrBadColor);
    CHECK(ParseWith("\"#FFFFFF\"", "16777215") == ThjErrBadColor);         // number
    CHECK(ParseWith("\"#FFFFFF\"", "99999999999") == ThjErrBadColor);      // out of range number
    CHECK(ParseWith("\"#FFFFFF\"", "-1") == ThjErrBadColor);
    CHECK(ParseWith("\"#FFFFFF\"", "null") == ThjErrBadColor);
    CHECK(ParseWith("\"#FFFFFF\"", "[\"#FFFFFF\"]") == ThjErrBadColor);
    CHECK(ParseWith("\"#FFFFFF\"", "\"#FFFFFF \"") == ThjErrBadColor);     // trailing space
    CHECK(ParseWith("\"text\": \"#FFFFFF\"", "\"nothing\": \"#FFFFFF\"") == ThjErrMissingColor);
    CHECK(ParseWith(",\n    \"highlight\": \"#808080\"", "") == ThjErrMissingColor);
    CHECK(ParseWith("\"colors\": {", "\"colors\": {\"text\": \"#FFFFFF\",") == ThjErrDuplicate);
    CHECK(ParseWith("\"colors\"", "\"colours\"") == ThjErrMissingColor);
    CHECK(ParseWith("\"colors\": {", "\"colors\": 5, \"x\": {") == ThjErrMissingColor);
    CHECK(ParseWith("\"name\": \"Test theme\",", "\"name\": \"a\", \"name\": \"b\",") == ThjErrDuplicate);
    CHECK(ParseWith("\"version\": 1,", "\"version\": 1, \"version\": 1,") == ThjErrDuplicate);

    // unreadable palette: text on background below 3:1
    CHECK(ParseWith("\"text\": \"#FFFFFF\"", "\"text\": \"#303030\"") == ThjErrContrast);
    CHECK(ParseWith("\"text\": \"#FFFFFF\"", "\"text\": \"#2B2B2B\"") == ThjErrContrast);
    CHECK(ParseWith("\"text\": \"#FFFFFF\"", "\"text\": \"#FFFFFF\"") == ThjOk);
    CHECK(ParseWith("\"background2\": \"#414141\"", "\"background2\": \"#F0F0F0\"") == ThjErrContrast); // white text on a near-white second background

    // syntax
    CHECK(ParseWith("\"version\": 1,", "\"version\": 1") == ThjErrSyntax); // missing comma
    CHECK(ParseWith("\"version\": 1,", "\"version\": 1,,") == ThjErrSyntax);
    CHECK(ParseWith("\"version\": 1,", "\"version\" 1,") == ThjErrSyntax);  // missing colon
    CHECK(ParseWith("\"version\": 1,", "'version': 1,") == ThjErrSyntax);   // single quotes
    CHECK(ParseWith("\"#FFFFFF\"", "\"#FFFFFF") == ThjErrSyntax);           // unterminated string (runs into the newline)
    CHECK(ParseWith("\"#2B2B2B\"", "\"#2B2B2B") == ThjErrBadColor);         // the next quote closes it, but the value is not a colour
    CHECK(ParseWith("\"#2B2B2B\"", "\"#2B2\\x2B\"") == ThjErrSyntax);       // bad escape
    CHECK(ParseWith("\"#2B2B2B\"", "\"#2B2\nB2B\"") == ThjErrSyntax);       // raw newline inside a string
    CHECK(ParseWith("\"#2B2B2B\"", "\"#2B2\\u12G4\"") == ThjErrSyntax);     // bad \\u escape
    CHECK(Parse("{\"format\":\"TaskManagerPlus.Theme\"} trailing") == ThjErrSyntax);
    CHECK(Parse("{\"format\":\"TaskManagerPlus.Theme\"}{}") == ThjErrSyntax);

    // size limit
    memset(big, ' ', sizeof(big));
    memcpy(big, Valid, strlen(Valid));
    CHECK(ThjParse(big, THJ_MAX_BYTES, &theme) == ThjOk);           // exactly at the limit, padded with spaces
    CHECK(ThjParse(big, THJ_MAX_BYTES + 1, &theme) == ThjErrTooLarge);

    // hostile nesting in an ignored key: bounded, no stack overflow
    deep[0] = 0;
    strcpy(deep, "{\"x\":");

    for (i = 0; i < 2000; i++)
        strcat(deep, "[");

    CHECK(Parse(deep) == ThjErrSyntax);
    strcpy(deep, "{\"x\":");

    for (i = 0; i < 1500; i++)
        strcat(deep, "{\"a\":");

    CHECK(Parse(deep) == ThjErrSyntax);

    // numbers
    CHECK(Parse("{\"x\":01}") == ThjErrSyntax);
    CHECK(Parse("{\"x\":1.}") == ThjErrSyntax);
    CHECK(Parse("{\"x\":-}") == ThjErrSyntax);
    CHECK(Parse("{\"x\":1e}") == ThjErrSyntax);
    CHECK(Parse("{\"x\":tru}") == ThjErrSyntax);
}

static void TestTruncationAndMutation(void)
{
    THJ_THEME theme;
    size_t length = strlen(Valid);
    size_t i;
    unsigned seed = 12345;
    int accepted = 0;

    // every proper prefix of a valid document is rejected (except the one that only drops the final newline)
    for (i = 0; i + 1 < length; i++)
    {
        char *copy = (char *)malloc(i ? i : 1); // exact size so a read past the end is caught by the CRT debug heap / ASan

        memcpy(copy, Valid, i);
        CHECK(ThjParse(copy, i, &theme) != ThjOk);
        free(copy);
    }

    // flipping each byte to a few values never crashes (a few mutations stay valid, e.g. a colour digit)
    for (i = 0; i < length; i++)
    {
        static const unsigned char values[] = { 0, '"', '{', '}', ',', ':', '\\', 0xFF, 'x', '#' };
        size_t v;

        for (v = 0; v < sizeof(values); v++)
        {
            char copy[2048];
            THJ_STATUS status;

            memcpy(copy, Valid, length);
            copy[i] = (char)values[v];
            status = ThjParse(copy, length, &theme);
            CHECK(status >= ThjOk && status <= ThjErrContrast);
            accepted += status == ThjOk;
        }
    }

    CHECK(accepted < (int)(length * 10)); // most mutations are rejected

    // deterministic random garbage and random token soup
    for (i = 0; i < 20000; i++)
    {
        char buffer[200];
        size_t n = (seed = seed * 1103515245u + 12345u) % sizeof(buffer);
        size_t j;
        static const char soup[] = "{}[],:\"\\ 0123456789-+.eEtruefalsnl#FformatTaskManagerPlus.Themeversioncolors";

        for (j = 0; j < n; j++)
        {
            seed = seed * 1103515245u + 12345u;
            buffer[j] = (i & 1) ? (char)(seed >> 16) : soup[(seed >> 16) % (sizeof(soup) - 1)];
        }

        CHECK(ThjParse(buffer, n, &theme) != ThjOk || n > 40);
    }
}

static void TestContrast(void)
{
    CHECK(ThjContrast(0x000000, 0xFFFFFF) > 20.9 && ThjContrast(0x000000, 0xFFFFFF) < 21.1);
    CHECK(ThjContrast(0xFFFFFF, 0xFFFFFF) > 0.99 && ThjContrast(0xFFFFFF, 0xFFFFFF) < 1.01);
    CHECK(ThjContrast(0x777777, 0xFFFFFF) > 4.4 && ThjContrast(0x777777, 0xFFFFFF) < 4.6); // WCAG: #777 on white is 4.48:1
    CHECK(ThjContrast(0xFFFFFF, 0x000000) == ThjContrast(0x000000, 0xFFFFFF));
}

static void TestColor(void)
{
    uint32_t rgb = 0;

    CHECK(ThjParseColor("#000000", 7, &rgb) && rgb == 0);
    CHECK(ThjParseColor("#FFFFFF", 7, &rgb) && rgb == 0xFFFFFFu);
    CHECK(ThjParseColor("#a1B2c3", 7, &rgb) && rgb == 0xA1B2C3u);
    CHECK(!ThjParseColor("#a1B2c3", 6, &rgb));
    CHECK(!ThjParseColor("#a1B2c3 ", 8, &rgb));
    CHECK(!ThjParseColor(NULL, 7, &rgb));
    CHECK(ThjParseColor("#000000", 7, NULL));
    CHECK(wcslen(ThjStatusText(ThjErrContrast)) > 10 && wcslen(ThjStatusText((THJ_STATUS)99)) > 5);
}

int main(void)
{
    TestValid();
    TestWriteRoundTrip();
    TestErrors();
    TestTruncationAndMutation();
    TestContrast();
    TestColor();
    printf("themejson: %d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
