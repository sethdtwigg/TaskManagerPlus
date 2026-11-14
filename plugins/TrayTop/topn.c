/*
 * TaskManagerPlus TrayTop: choosing the top N processes by CPU or by memory (see topn.h).
 */

#include "topn.h"

unsigned TtClampCount(long long Count)
{
    if (Count < TT_COUNT_MIN)
        return Count == 0 ? TT_COUNT_DEFAULT : TT_COUNT_MIN;

    if (Count > TT_COUNT_MAX)
        return TT_COUNT_MAX;

    return (unsigned)Count;
}

/** 1 when A ranks ahead of B for the chosen sort. */
static int TtBefore(const TT_ITEM *A, const TT_ITEM *B, int ByMemory)
{
    if (ByMemory)
    {
        if (A->Memory != B->Memory)
            return A->Memory > B->Memory;
    }
    else
    {
        if (A->Cpu != B->Cpu)
            return A->Cpu > B->Cpu;
    }

    return A->Pid < B->Pid;
}

static int TtListable(const TT_ITEM *Item, int ByMemory)
{
    if (Item->Pid == 0)
        return 0;

    if (ByMemory)
        return Item->Memory > 0;

    return Item->Cpu > 0.05; // NaN fails this too
}

size_t TtSelectTop(const TT_ITEM *Items, size_t Count, int ByMemory, size_t N, size_t *OutIndexes)
{
    size_t written = 0;
    size_t i;

    if (!Items || !OutIndexes || !N)
        return 0;

    for (i = 0; i < Count; i++)
    {
        size_t position;

        if (!TtListable(&Items[i], ByMemory))
            continue;

        // insertion into the sorted output, which never holds more than N entries
        position = written;

        while (position > 0 && TtBefore(&Items[i], &Items[OutIndexes[position - 1]], ByMemory))
            position--;

        if (position >= N)
            continue; // heavier than nothing in a full list? no: it ranks after all N

        if (written < N)
            written++;

        {
            size_t j;

            for (j = written - 1; j > position; j--)
                OutIndexes[j] = OutIndexes[j - 1];
        }

        OutIndexes[position] = i;
    }

    return written;
}
