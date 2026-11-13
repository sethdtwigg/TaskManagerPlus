/*
 * TaskManagerPlus ProcessRules: share codes. See sharecode.h.
 */

#include "sharecode.h"
#include <stdlib.h>
#include <string.h>

static const char ScAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t ScCrc32(const uint8_t *Data, size_t Length)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < Length; i++)
    {
        crc ^= Data[i];

        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }

    return ~crc;
}

size_t ScEncodedSize(size_t Length)
{
    return strlen(SC_PREFIX) + ((Length + 2) / 3) * 4 + 1 /* . */ + 8 + 1 /* NUL */;
}

int ScEncode(const uint8_t *Data, size_t Length, char *Out, size_t Capacity)
{
    size_t i;
    size_t n = 0;
    uint32_t crc;
    static const char hex[] = "0123456789abcdef";

    if (Length > SC_MAX_PAYLOAD || Capacity < ScEncodedSize(Length))
        return 0;

    memcpy(Out, SC_PREFIX, strlen(SC_PREFIX));
    n = strlen(SC_PREFIX);

    for (i = 0; i < Length; i += 3)
    {
        uint32_t v = (uint32_t)Data[i] << 16;
        size_t rest = Length - i;

        if (rest > 1)
            v |= (uint32_t)Data[i + 1] << 8;

        if (rest > 2)
            v |= Data[i + 2];

        Out[n++] = ScAlphabet[(v >> 18) & 63];
        Out[n++] = ScAlphabet[(v >> 12) & 63];
        Out[n++] = rest > 1 ? ScAlphabet[(v >> 6) & 63] : '=';
        Out[n++] = rest > 2 ? ScAlphabet[v & 63] : '=';
    }

    crc = ScCrc32(Data, Length);
    Out[n++] = '.';

    for (i = 0; i < 8; i++)
        Out[n++] = hex[(crc >> (28 - 4 * i)) & 15];

    Out[n] = 0;
    return 1;
}

static int ScValue(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int ScHexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ScDecode(const char *Code, size_t CodeLength, uint8_t **Data, size_t *Length, const char **Reason)
{
    char *text;
    size_t n = 0;
    size_t i;
    size_t prefix = strlen(SC_PREFIX);
    char *dot;
    size_t b64Length;
    uint8_t *out;
    size_t outLength = 0;
    uint32_t expected = 0;
    uint32_t acc = 0;
    int bits = 0;
    int pad = 0;

    *Data = NULL;
    *Length = 0;

    if (!Code || !CodeLength)
    {
        *Reason = "The code is empty.";
        return 0;
    }

    if (CodeLength > SC_MAX_PAYLOAD * 2)
    {
        *Reason = "The code is too long.";
        return 0;
    }

    text = (char *)malloc(CodeLength + 1);

    if (!text)
    {
        *Reason = "Out of memory.";
        return 0;
    }

    for (i = 0; i < CodeLength; i++)
    {
        unsigned char c = (unsigned char)Code[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;

        text[n++] = (char)c;
    }

    text[n] = 0;

    if (n < prefix + 9 || strncmp(text, SC_PREFIX, prefix))
    {
        free(text);
        *Reason = "That is not a TaskManagerPlus rules code (it must start with TMPR1:).";
        return 0;
    }

    dot = strrchr(text, '.');

    if (!dot || strlen(dot) != 9)
    {
        free(text);
        *Reason = "The code is incomplete (the checksum at the end is missing).";
        return 0;
    }

    for (i = 0; i < 8; i++)
    {
        int h = ScHexValue(dot[1 + i]);

        if (h < 0)
        {
            free(text);
            *Reason = "The checksum at the end of the code is not valid.";
            return 0;
        }

        expected = (expected << 4) | (uint32_t)h;
    }

    b64Length = (size_t)(dot - (text + prefix));

    if (b64Length == 0 || b64Length % 4 != 0)
    {
        free(text);
        *Reason = "The code is incomplete or damaged.";
        return 0;
    }

    out = (uint8_t *)malloc(b64Length / 4 * 3 + 1);

    if (!out)
    {
        free(text);
        *Reason = "Out of memory.";
        return 0;
    }

    for (i = 0; i < b64Length; i++)
    {
        char c = text[prefix + i];
        int v;

        if (c == '=')
        {
            // padding only in the last two positions
            if (i < b64Length - 2)
            {
                free(text);
                free(out);
                *Reason = "The code is damaged (misplaced padding).";
                return 0;
            }

            pad++;
            continue;
        }

        if (pad)
        {
            free(text);
            free(out);
            *Reason = "The code is damaged (data after padding).";
            return 0;
        }

        v = ScValue(c);

        if (v < 0)
        {
            free(text);
            free(out);
            *Reason = "The code contains characters that are not allowed.";
            return 0;
        }

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            out[outLength++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }

    free(text);

    if (outLength > SC_MAX_PAYLOAD)
    {
        free(out);
        *Reason = "The code holds too much data.";
        return 0;
    }

    if (ScCrc32(out, outLength) != expected)
    {
        free(out);
        *Reason = "The checksum does not match: the code was cut off or changed.";
        return 0;
    }

    out[outLength] = 0;
    *Data = out;
    *Length = outLength;
    *Reason = "";
    return 1;
}

void ScFree(uint8_t *Data)
{
    free(Data);
}
