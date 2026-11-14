/*
 * TaskManagerPlus TrayTop: choosing the top N processes by CPU or by memory.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 */

#ifndef _TT_TOPN_H_
#define _TT_TOPN_H_

#include <stddef.h>
#include <stdint.h>

#define TT_COUNT_MIN 1
#define TT_COUNT_MAX 15
#define TT_COUNT_DEFAULT 5

typedef struct _TT_ITEM
{
    uint32_t Pid;
    double Cpu;         // percent, 0..100 * cores
    uint64_t Memory;    // bytes (working set)
} TT_ITEM;

#ifdef __cplusplus
extern "C" {
#endif

/** Clamps a stored count into TT_COUNT_MIN..TT_COUNT_MAX; anything unreadable (zero, negative, huge) gives the default or the nearest bound. */
unsigned TtClampCount(long long Count);

/**
 * Picks the N heaviest items by CPU (ByMemory 0) or by memory (1) into OutIndexes (room for N), heaviest first.
 * Pid 0 (System Idle) is never listed; an item with nothing to show (CPU of 0 or not above 0.05, no memory) is skipped, so
 * an idle machine gives a short list. Ties break by the lower Pid so the order is stable. Returns how many were written.
 */
size_t TtSelectTop(const TT_ITEM *Items, size_t Count, int ByMemory, size_t N, size_t *OutIndexes);

#ifdef __cplusplus
}
#endif

#endif
