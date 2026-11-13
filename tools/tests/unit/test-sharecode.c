/*
 * Unit tests for the rules share code (small gap, Phase 7): encode / decode round trip, checksum, whitespace,
 * truncation at every length, damaged and hostile input.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../plugins/ProcessRules/sharecode.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static char *Encode(const char *text)
{
    size_t len = strlen(text);
    size_t cap = ScEncodedSize(len);
    char *out = (char *)malloc(cap);

    CHECK(ScEncode((const uint8_t *)text, len, out, cap));
    return out;
}

static void TestRoundTrip(void)
{
    static const char *samples[] =
    {
        "x", "ab", "abc", "abcd", "{\"version\":1,\"rules\":[]}",
        "{\"rules\":[{\"program\":\"caf\xC3\xA9.exe\",\"path\":\"C:\\\\a b\\\\\"}]}"
    };
    size_t i;
    uint8_t *data;
    size_t length;
    const char *reason;

    CHECK(ScCrc32((const uint8_t *)"123456789", 9) == 0xCBF43926u); // the standard CRC-32 check value
    CHECK(ScCrc32((const uint8_t *)"", 0) == 0);

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
    {
        char *code = Encode(samples[i]);

        CHECK(!strncmp(code, "TMPR1:", 6));
        CHECK(ScDecode(code, strlen(code), &data, &length, &reason));
        CHECK(length == strlen(samples[i]) && !memcmp(data, samples[i], length) && data[length] == 0);
        ScFree(data);
        free(code);
    }

    // binary payload with every byte value
    {
        uint8_t all[256];
        char *code;
        int b;

        for (b = 0; b < 256; b++)
            all[b] = (uint8_t)b;

        code = (char *)malloc(ScEncodedSize(256));
        CHECK(ScEncode(all, 256, code, ScEncodedSize(256)));
        CHECK(ScDecode(code, strlen(code), &data, &length, &reason) && length == 256 && !memcmp(data, all, 256));
        ScFree(data);
        free(code);
    }

    // too small an output buffer, over the size limit
    {
        char small[8];

        CHECK(!ScEncode((const uint8_t *)"abc", 3, small, sizeof(small)));
        CHECK(!ScEncode((const uint8_t *)"abc", SC_MAX_PAYLOAD + 1, small, sizeof(small)));
    }
}

static void TestTolerance(void)
{
    char *code = Encode("{\"version\":1,\"rules\":[{\"program\":\"chrome.exe\"}]}");
    size_t len = strlen(code);
    char *wrapped = (char *)malloc(len * 2 + 8);
    size_t i, n = 0;
    uint8_t *data;
    size_t length;
    const char *reason;

    // whitespace and line breaks anywhere (a code wrapped by a chat client or padded with spaces)
    wrapped[n++] = ' ';
    wrapped[n++] = '\n';

    for (i = 0; i < len; i++)
    {
        wrapped[n++] = code[i];

        if (i % 17 == 16)
        {
            wrapped[n++] = '\r';
            wrapped[n++] = '\n';
        }

        if (i % 29 == 5)
            wrapped[n++] = ' ';
    }

    wrapped[n++] = '\t';
    wrapped[n] = 0;
    CHECK(ScDecode(wrapped, n, &data, &length, &reason) && length > 10);
    ScFree(data);
    free(wrapped);
    free(code);
}

static void TestRejects(void)
{
    char *code = Encode("{\"version\":1,\"rules\":[{\"program\":\"chrome.exe\"},{\"program\":\"game.exe\"}]}");
    size_t len = strlen(code);
    uint8_t *data;
    size_t length;
    const char *reason;
    size_t n;
    char *copy = (char *)malloc(len + 2);

    // cut off at every length: never accepted, never crashes
    for (n = 0; n < len; n++)
    {
        memcpy(copy, code, n);
        copy[n] = 0;
        CHECK(!ScDecode(copy, n, &data, &length, &reason) && data == NULL);
    }

    // one character changed in the payload: checksum catches it
    memcpy(copy, code, len + 1);
    copy[10] = copy[10] == 'A' ? 'B' : 'A';
    CHECK(!ScDecode(copy, len, &data, &length, &reason) && strstr(reason, "checksum"));

    // one character changed in the checksum
    memcpy(copy, code, len + 1);
    copy[len - 1] = copy[len - 1] == '0' ? '1' : '0';
    CHECK(!ScDecode(copy, len, &data, &length, &reason));

    // wrong prefix, other version, no prefix
    memcpy(copy, code, len + 1);
    copy[4] = 'X';
    CHECK(!ScDecode(copy, len, &data, &length, &reason) && strstr(reason, "TMPR1"));
    memcpy(copy, code, len + 1);
    copy[5] = '2';
    CHECK(!ScDecode(copy, len, &data, &length, &reason));
    CHECK(!ScDecode(code + 6, len - 6, &data, &length, &reason));

    // junk and degenerate input
    CHECK(!ScDecode("", 0, &data, &length, &reason));
    CHECK(!ScDecode(NULL, 5, &data, &length, &reason));
    CHECK(!ScDecode("TMPR1:", 6, &data, &length, &reason));
    CHECK(!ScDecode("TMPR1:.00000000", 15, &data, &length, &reason));
    CHECK(!ScDecode("TMPR1:AAAA.zzzzzzzz", 19, &data, &length, &reason));
    CHECK(!ScDecode("TMPR1:AA=A.00000000", 19, &data, &length, &reason)); // padding in the middle
    CHECK(!ScDecode("TMPR1:A$AA.00000000", 19, &data, &length, &reason)); // outside the alphabet
    CHECK(!ScDecode("<script>alert(1)</script>", 25, &data, &length, &reason));
    CHECK(!ScDecode("TMPR1:AAAA", 10, &data, &length, &reason)); // no checksum

    // a huge string is refused before any allocation of that size is attempted
    {
        size_t big = SC_MAX_PAYLOAD * 2 + 100;
        char *huge = (char *)malloc(big + 1);

        memset(huge, 'A', big);
        huge[big] = 0;
        memcpy(huge, "TMPR1:", 6);
        CHECK(!ScDecode(huge, big, &data, &length, &reason));
        free(huge);
    }

    free(copy);
    free(code);
}

int main(void)
{
    TestRoundTrip();
    TestTolerance();
    TestRejects();
    printf("sharecode: %d checks, %d failed\n", Checks, Failures);
    return Failures ? 1 : 0;
}
