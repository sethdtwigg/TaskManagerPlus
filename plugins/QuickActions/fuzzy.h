/*
 * TaskManagerPlus QuickActions: fuzzy matcher.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 */

#ifndef _QA_FUZZY_H_
#define _QA_FUZZY_H_

#include <stddef.h>

#define QA_NO_MATCH (-2147483647L - 1L)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scores Text against Pattern. The pattern is split on spaces; every word must appear in the text as a
 * case-insensitive subsequence, and the scores add up. Higher is better: consecutive characters, characters at the
 * start of a word and a prefix or exact word match score more, gaps cost. An empty pattern matches everything with 0.
 *
 * \return QA_NO_MATCH when some word does not match, otherwise the score.
 */
long QaFuzzyScore(
    const wchar_t *Pattern,
    const wchar_t *Text
    );

#ifdef __cplusplus
}
#endif

#endif
