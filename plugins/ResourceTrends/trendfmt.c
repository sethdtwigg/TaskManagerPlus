/*
 * TaskManagerPlus ResourceTrends: trend file format, thinning and aggregation (see trendfmt.h).
 */

#include "trendfmt.h"
#include <stdlib.h>
#include <string.h>

static void TrPut16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void TrPut32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void TrPut64(uint8_t *p, uint64_t v)
{
    TrPut32(p, (uint32_t)v);
    TrPut32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t TrGet16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t TrGet32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t TrGet64(const uint8_t *p)
{
    return (uint64_t)TrGet32(p) | ((uint64_t)TrGet32(p + 4) << 32);
}

uint32_t TrEncodeHeader(uint8_t *Out, uint32_t Capacity)
{
    if (Capacity < TR_HEADER_BYTES)
        return 0;

    TrPut32(Out, TR_MAGIC);
    TrPut32(Out + 4, TR_VERSION);
    TrPut32(Out + 8, 0);
    TrPut32(Out + 12, 0);
    return TR_HEADER_BYTES;
}

uint32_t TrEncodeString(uint8_t *Out, uint32_t Capacity, const uint16_t *Text, uint32_t Chars)
{
    uint32_t total = TR_RECORD_HEAD_BYTES + Chars * 2;
    uint32_t i;

    if (Chars > 4096 || Capacity < total)
        return 0;

    TrPut32(Out, TR_TYPE_STRING);
    TrPut32(Out + 4, Chars * 2);

    for (i = 0; i < Chars; i++)
        TrPut16(Out + TR_RECORD_HEAD_BYTES + i * 2, Text[i]);

    return total;
}

uint32_t TrEncodeSampleSize(uint32_t Count)
{
    return TR_RECORD_HEAD_BYTES + TR_SAMPLE_HEAD_BYTES + Count * TR_PROCESS_BYTES;
}

uint32_t TrEncodeSample(uint8_t *Out, uint32_t Capacity, const TR_SYSTEM *System, const TR_PROCESS *Processes, uint32_t Count)
{
    uint32_t total = TrEncodeSampleSize(Count);
    uint8_t *p;
    uint32_t i;

    if (Count > 100000 || Capacity < total)
        return 0;

    TrPut32(Out, TR_TYPE_SAMPLE);
    TrPut32(Out + 4, total - TR_RECORD_HEAD_BYTES);
    p = Out + TR_RECORD_HEAD_BYTES;
    TrPut64(p, (uint64_t)System->Time);
    TrPut32(p + 8, System->CpuX100);
    TrPut32(p + 12, System->KernelX100);
    TrPut32(p + 16, System->PhysicalUsedMB);
    TrPut32(p + 20, System->PhysicalTotalMB);
    TrPut32(p + 24, System->CommitUsedMB);
    TrPut32(p + 28, System->CommitLimitMB);
    TrPut32(p + 32, Count);
    TrPut32(p + 36, 0);
    p += TR_SAMPLE_HEAD_BYTES;

    for (i = 0; i < Count; i++, p += TR_PROCESS_BYTES)
    {
        TrPut32(p, Processes[i].ProcessId);
        TrPut32(p + 4, Processes[i].NameIndex);
        TrPut64(p + 8, (uint64_t)Processes[i].CreateTime);
        TrPut32(p + 16, Processes[i].PrivateKB);
        TrPut32(p + 20, Processes[i].WorkingSetKB);
        TrPut16(p + 24, Processes[i].CpuX100);
        TrPut16(p + 26, Processes[i].Flags);
    }

    return total;
}

int TrParse(const uint8_t *Data, size_t Size, const TR_READER *Reader, size_t *ValidBytes)
{
    size_t offset = TR_HEADER_BYTES;
    int records = 0;
    TR_PROCESS *processes = NULL;
    uint32_t capacity = 0;

    if (ValidBytes)
        *ValidBytes = 0;

    if (Size < TR_HEADER_BYTES || TrGet32(Data) != TR_MAGIC || TrGet32(Data + 4) != TR_VERSION)
        return -1;

    if (ValidBytes)
        *ValidBytes = offset;

    while (offset + TR_RECORD_HEAD_BYTES <= Size)
    {
        uint32_t type = TrGet32(Data + offset);
        uint32_t length = TrGet32(Data + offset + 4);
        const uint8_t *payload = Data + offset + TR_RECORD_HEAD_BYTES;

        if (length > Size - offset - TR_RECORD_HEAD_BYTES)
            break; // truncated tail

        if (type == TR_TYPE_STRING && length % 2 == 0)
        {
            if (Reader->String && !Reader->String(Reader->Context, payload, length / 2))
                break;
        }
        else if (type == TR_TYPE_SAMPLE && length >= TR_SAMPLE_HEAD_BYTES)
        {
            uint32_t count = TrGet32(payload + 32);

            if (count <= 100000 && length == TR_SAMPLE_HEAD_BYTES + count * TR_PROCESS_BYTES)
            {
                TR_SYSTEM system;
                uint32_t i;

                system.Time = (int64_t)TrGet64(payload);
                system.CpuX100 = TrGet32(payload + 8);
                system.KernelX100 = TrGet32(payload + 12);
                system.PhysicalUsedMB = TrGet32(payload + 16);
                system.PhysicalTotalMB = TrGet32(payload + 20);
                system.CommitUsedMB = TrGet32(payload + 24);
                system.CommitLimitMB = TrGet32(payload + 28);

                if (count > capacity)
                {
                    TR_PROCESS *grown = realloc(processes, count * sizeof(TR_PROCESS));

                    if (!grown)
                        break;

                    processes = grown;
                    capacity = count;
                }

                for (i = 0; i < count; i++)
                {
                    const uint8_t *p = payload + TR_SAMPLE_HEAD_BYTES + i * TR_PROCESS_BYTES;

                    processes[i].ProcessId = TrGet32(p);
                    processes[i].NameIndex = TrGet32(p + 4);
                    processes[i].CreateTime = (int64_t)TrGet64(p + 8);
                    processes[i].PrivateKB = TrGet32(p + 16);
                    processes[i].WorkingSetKB = TrGet32(p + 20);
                    processes[i].CpuX100 = TrGet16(p + 24);
                    processes[i].Flags = TrGet16(p + 26);
                }

                if (Reader->Sample && !Reader->Sample(Reader->Context, &system, processes, count))
                    break;
            }
        }
        // unknown types and malformed samples are skipped

        offset += TR_RECORD_HEAD_BYTES + length;
        records++;

        if (ValidBytes)
            *ValidBytes = offset;
    }

    free(processes);
    return records;
}

uint32_t TrThin(const int64_t *Times, uint32_t Count, int64_t Now, uint8_t *Keep)
{
    uint32_t i;
    uint32_t kept = 0;
    int64_t lastBucket = -1;
    int64_t lastWidth = 0;

    for (i = 0; i < Count; i++)
    {
        int64_t age = Now - Times[i];
        int64_t width;
        int64_t bucket;

        if (age < 6 * TR_FILETIME_HOUR)
            width = 0;
        else if (age < 48 * TR_FILETIME_HOUR)
            width = 5 * TR_FILETIME_MINUTE;
        else
            width = 30 * TR_FILETIME_MINUTE;

        if (width == 0)
        {
            Keep[i] = 1;
            kept++;
            lastBucket = -1;
            lastWidth = 0;
            continue;
        }

        bucket = Times[i] / width;

        if (width == lastWidth && bucket == lastBucket)
        {
            Keep[i] = 0;
        }
        else
        {
            Keep[i] = 1;
            kept++;
            lastBucket = bucket;
            lastWidth = width;
        }
    }

    return kept;
}

typedef struct _TR_ACC
{
    TR_AGGREGATE Row;
    double CpuSum;
    double PrivateSum;
} TR_ACC;

static int TrCompareKey(const void *A, const void *B)
{
    const TR_ACC *a = A;
    const TR_ACC *b = B;

    if (a->Row.ProcessId != b->Row.ProcessId)
        return a->Row.ProcessId < b->Row.ProcessId ? -1 : 1;

    if (a->Row.CreateTime != b->Row.CreateTime)
        return a->Row.CreateTime < b->Row.CreateTime ? -1 : 1;

    return 0;
}

static int TrCompareCpu(const void *A, const void *B)
{
    const TR_AGGREGATE *a = A;
    const TR_AGGREGATE *b = B;

    if (a->AvgCpuPercent != b->AvgCpuPercent)
        return a->AvgCpuPercent < b->AvgCpuPercent ? 1 : -1;

    if (a->PeakPrivateKB != b->PeakPrivateKB)
        return a->PeakPrivateKB < b->PeakPrivateKB ? 1 : -1;

    return 0;
}

static int TrCompareMemory(const void *A, const void *B)
{
    const TR_AGGREGATE *a = A;
    const TR_AGGREGATE *b = B;

    if (a->PeakPrivateKB != b->PeakPrivateKB)
        return a->PeakPrivateKB < b->PeakPrivateKB ? 1 : -1;

    if (a->AvgCpuPercent != b->AvgCpuPercent)
        return a->AvgCpuPercent < b->AvgCpuPercent ? 1 : -1;

    return 0;
}

uint32_t TrAggregate(
    TR_SAMPLE *const *Samples,
    uint32_t Count,
    int64_t From,
    int64_t To,
    int Rank,
    TR_AGGREGATE **Rows,
    uint32_t *SamplesInRange
    )
{
    uint32_t i;
    uint32_t j;
    uint32_t inRange = 0;
    uint32_t total = 0;
    TR_ACC *all;
    TR_AGGREGATE *out;
    struct { double Cpu; double Private; } *sums;
    uint32_t rows = 0;

    *Rows = NULL;
    *SamplesInRange = 0;

    for (i = 0; i < Count; i++)
    {
        if (Samples[i]->System.Time >= From && Samples[i]->System.Time <= To)
        {
            inRange++;
            total += Samples[i]->Count;
        }
    }

    *SamplesInRange = inRange;

    if (!inRange || !total)
        return 0;

    all = malloc(total * sizeof(TR_ACC));

    if (!all)
        return 0;

    total = 0;

    for (i = 0; i < Count; i++)
    {
        if (Samples[i]->System.Time < From || Samples[i]->System.Time > To)
            continue;

        for (j = 0; j < Samples[i]->Count; j++)
        {
            const TR_PROCESS *p = &Samples[i]->Processes[j];
            TR_ACC *a = &all[total++];

            memset(a, 0, sizeof(TR_ACC));
            a->Row.ProcessId = p->ProcessId;
            a->Row.NameIndex = p->NameIndex;
            a->Row.CreateTime = p->CreateTime;
            a->Row.PeakCpuPercent = p->CpuX100 / 100.0;
            a->Row.PeakPrivateKB = p->PrivateKB;
            a->Row.PeakWorkingSetKB = p->WorkingSetKB;
            a->Row.Samples = 1;
            a->CpuSum = p->CpuX100 / 100.0;
            a->PrivateSum = p->PrivateKB;
        }
    }

    qsort(all, total, sizeof(TR_ACC), TrCompareKey);
    out = malloc(total * sizeof(TR_AGGREGATE));
    sums = malloc(total * sizeof(*sums));

    if (!out || !sums)
    {
        free(out);
        free(sums);
        free(all);
        return 0;
    }

    for (i = 0; i < total; i++)
    {
        if (rows && out[rows - 1].ProcessId == all[i].Row.ProcessId && out[rows - 1].CreateTime == all[i].Row.CreateTime)
        {
            TR_AGGREGATE *r = &out[rows - 1];

            r->Samples++;
            if (all[i].Row.PeakCpuPercent > r->PeakCpuPercent)
                r->PeakCpuPercent = all[i].Row.PeakCpuPercent;
            if (all[i].Row.PeakPrivateKB > r->PeakPrivateKB)
                r->PeakPrivateKB = all[i].Row.PeakPrivateKB;
            if (all[i].Row.PeakWorkingSetKB > r->PeakWorkingSetKB)
                r->PeakWorkingSetKB = all[i].Row.PeakWorkingSetKB;
            sums[rows - 1].Cpu += all[i].CpuSum;
            sums[rows - 1].Private += all[i].PrivateSum;
        }
        else
        {
            out[rows] = all[i].Row;
            sums[rows].Cpu = all[i].CpuSum;
            sums[rows].Private = all[i].PrivateSum;
            rows++;
        }
    }

    for (i = 0; i < rows; i++)
    {
        out[i].AvgCpuPercent = sums[i].Cpu / inRange; // absent samples count as 0
        out[i].AvgPrivateKB = (uint32_t)(sums[i].Private / out[i].Samples);
    }

    free(sums);
    free(all);
    qsort(out, rows, sizeof(TR_AGGREGATE), Rank == 1 ? TrCompareMemory : TrCompareCpu);
    *Rows = out;
    return rows;
}

void TrFreeAggregate(TR_AGGREGATE *Rows)
{
    free(Rows);
}
