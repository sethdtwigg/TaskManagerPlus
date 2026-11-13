/*
 * TaskManagerPlus QuickActions: fuzzy matcher (see fuzzy.h).
 */

#include "fuzzy.h"

#define QA_MAX_TEXT 255
#define QA_NEG (-1000000L)
#define QA_BONUS_CONSECUTIVE 16
#define QA_BONUS_WORD_START 12
#define QA_BONUS_FIRST_CHAR 6
#define QA_GAP_PENALTY 2
#define QA_LEADING_GAP_MAX 6

static wchar_t QaFold(
    wchar_t Character
    )
{
    if (Character >= L'A' && Character <= L'Z')
        return (wchar_t)(Character + (L'a' - L'A'));

    // Latin-1 supplement and Greek/Cyrillic upper case would need tables; the names searched are mostly ASCII.
    if (Character >= 0xC0 && Character <= 0xDE && Character != 0xD7)
        return (wchar_t)(Character + 0x20);

    return Character;
}

static int QaIsSeparator(
    wchar_t Character
    )
{
    return Character == L' ' || Character == L'_' || Character == L'-' || Character == L'.' ||
        Character == L'/' || Character == L'\\' || Character == L'(' || Character == L')' ||
        Character == L':' || Character == L',';
}

/** Score of one word (no spaces) against Text of Length characters, or QA_NO_MATCH. */
static long QaScoreWord(
    const wchar_t *Word,
    size_t WordLength,
    const wchar_t *Text,
    size_t Length
    )
{
    long bonus[QA_MAX_TEXT];
    long matchRow[2][QA_MAX_TEXT]; // M: best score for the word prefix within Text[0..j]
    long endRow[2][QA_MAX_TEXT]; // D: best score with the word prefix ending exactly at Text[j]
    wchar_t lower[QA_MAX_TEXT];
    size_t i;
    size_t j;
    size_t previous = 0;
    size_t current = 1;
    size_t scan;
    long result;

    if (WordLength == 0)
        return 0;

    if (Length > QA_MAX_TEXT)
        Length = QA_MAX_TEXT;

    if (WordLength > Length)
        return QA_NO_MATCH;

    // Subsequence prefilter: cheap and rejects most candidates.
    scan = 0;

    for (j = 0; j < Length && scan < WordLength; j++)
    {
        if (QaFold(Text[j]) == QaFold(Word[scan]))
            scan++;
    }

    if (scan != WordLength)
        return QA_NO_MATCH;

    // Tighten the window backwards from the end of the leftmost match. A word of 3 or more characters whose
    // characters are scattered over far more text than the word is long ("chrome" across "End process tree
    // vmcompute.exe kill ...") is not a match.
    if (WordLength >= 3)
    {
        size_t end = j - 1;
        size_t back = WordLength;
        size_t start = end + 1;

        while (back > 0 && start > 0)
        {
            start--;

            if (QaFold(Text[start]) == QaFold(Word[back - 1]))
                back--;
        }

        if (end - start + 1 > WordLength * 2 + 4)
            return QA_NO_MATCH;
    }

    for (j = 0; j < Length; j++)
    {
        wchar_t c = Text[j];

        lower[j] = QaFold(c);

        if (j == 0)
            bonus[j] = QA_BONUS_WORD_START + QA_BONUS_FIRST_CHAR;
        else if (QaIsSeparator(Text[j - 1]))
            bonus[j] = QA_BONUS_WORD_START;
        else if (c >= L'A' && c <= L'Z' && Text[j - 1] >= L'a' && Text[j - 1] <= L'z')
            bonus[j] = QA_BONUS_WORD_START - 2; // camelCase boundary
        else if (c >= L'0' && c <= L'9' && !(Text[j - 1] >= L'0' && Text[j - 1] <= L'9'))
            bonus[j] = QA_BONUS_WORD_START - 4;
        else
            bonus[j] = 0;
    }

    for (i = 0; i < WordLength; i++)
    {
        wchar_t w = QaFold(Word[i]);

        for (j = 0; j < Length; j++)
        {
            long end = QA_NEG;
            long match;

            if (lower[j] == w)
            {
                if (i == 0)
                {
                    long leading = (long)j;

                    if (leading > QA_LEADING_GAP_MAX)
                        leading = QA_LEADING_GAP_MAX;

                    end = bonus[j] - leading;
                }
                else if (j > 0)
                {
                    long viaGap = matchRow[previous][j - 1] > QA_NEG ? matchRow[previous][j - 1] + bonus[j] : QA_NEG;
                    long viaRun = endRow[previous][j - 1] > QA_NEG ? endRow[previous][j - 1] + QA_BONUS_CONSECUTIVE : QA_NEG;

                    end = viaGap > viaRun ? viaGap : viaRun;
                }
            }

            endRow[current][j] = end;
            match = end;

            if (j > 0 && matchRow[current][j - 1] > QA_NEG && matchRow[current][j - 1] - QA_GAP_PENALTY > match)
                match = matchRow[current][j - 1] - QA_GAP_PENALTY;

            matchRow[current][j] = match;
        }

        previous = current;
        current = 1 - current;
    }

    result = matchRow[previous][Length - 1];

    if (result <= QA_NEG)
        return QA_NO_MATCH;

    // Prefix and exact bonuses.
    {
        size_t k;

        for (k = 0; k < WordLength && k < Length; k++)
        {
            if (lower[k] != QaFold(Word[k]))
                break;
        }

        if (k == WordLength)
        {
            result += 40;

            if (Length == WordLength)
                result += 60;
        }
    }

    return result;
}

long QaFuzzyScore(
    const wchar_t *Pattern,
    const wchar_t *Text
    )
{
    long total = 0;
    size_t textLength = 0;

    if (!Pattern)
        return 0;

    if (!Text)
        Text = L"";

    while (Text[textLength])
        textLength++;

    while (*Pattern)
    {
        const wchar_t *start;
        size_t length;
        long score;

        while (*Pattern == L' ')
            Pattern++;

        if (!*Pattern)
            break;

        start = Pattern;

        while (*Pattern && *Pattern != L' ')
            Pattern++;

        length = (size_t)(Pattern - start);
        score = QaScoreWord(start, length, Text, textLength);

        if (score == QA_NO_MATCH)
            return QA_NO_MATCH;

        total += score;
    }

    return total;
}
