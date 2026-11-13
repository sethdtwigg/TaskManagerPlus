/*
 * TaskManagerPlus ResourceTrends: trend file format, thinning and range aggregation.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 *
 * File tmp-trends.bin, all integers little endian:
 *   header (16 bytes): u32 magic 0x52544D54 (bytes 54 4D 54 52 = "TMTR"), u32 version (1), u32 0, u32 0
 *   records:           u32 type, u32 payload length, payload
 *     type 1 string:   UTF-16LE characters, no terminator. Strings get indices 0,1,2... in file order.
 *     type 2 sample:   40-byte head + count * 28-byte process records
 *       head: i64 time (FILETIME, UTC), u32 systemCpu (hundredths of a percent, 0..10000),
 *             u32 kernelCpu (same), u32 physicalUsedMB, u32 physicalTotalMB, u32 commitUsedMB,
 *             u32 commitLimitMB, u32 count, u32 reserved
 *       process: u32 pid, u32 nameIndex, i64 createTime, u32 privateKB, u32 workingSetKB,
 *                u16 cpu (hundredths of a percent of the whole system), u16 flags
 *   Append-only. A reader stops at a truncated record, skips unknown types and malformed samples.
 */

#ifndef _TR_TRENDFMT_H_
#define _TR_TRENDFMT_H_

#include <stdint.h>
#include <stddef.h>

#define TR_MAGIC 0x52544D54u
#define TR_VERSION 1u
#define TR_HEADER_BYTES 16u
#define TR_RECORD_HEAD_BYTES 8u
#define TR_SAMPLE_HEAD_BYTES 40u
#define TR_PROCESS_BYTES 28u
#define TR_TYPE_STRING 1u
#define TR_TYPE_SAMPLE 2u

#define TR_FILETIME_SECOND 10000000LL
#define TR_FILETIME_MINUTE (60LL * TR_FILETIME_SECOND)
#define TR_FILETIME_HOUR (60LL * TR_FILETIME_MINUTE)

typedef struct _TR_SYSTEM
{
    int64_t Time;
    uint32_t CpuX100;
    uint32_t KernelX100;
    uint32_t PhysicalUsedMB;
    uint32_t PhysicalTotalMB;
    uint32_t CommitUsedMB;
    uint32_t CommitLimitMB;
} TR_SYSTEM;

typedef struct _TR_PROCESS
{
    uint32_t ProcessId;
    uint32_t NameIndex;
    int64_t CreateTime;
    uint32_t PrivateKB;
    uint32_t WorkingSetKB;
    uint16_t CpuX100;
    uint16_t Flags;
} TR_PROCESS;

/** A sample as held in memory; Processes is owned by the sample (malloc). */
typedef struct _TR_SAMPLE
{
    TR_SYSTEM System;
    uint32_t Count;
    TR_PROCESS *Processes;
} TR_SAMPLE;

typedef struct _TR_READER
{
    void *Context;
    /** Text is UTF-16LE, Chars characters, not terminated and possibly unaligned. Return 0 to stop. */
    int (*String)(void *Context, const uint8_t *Text, uint32_t Chars);
    /** Processes is a copy valid only during the call. Return 0 to stop. */
    int (*Sample)(void *Context, const TR_SYSTEM *System, const TR_PROCESS *Processes, uint32_t Count);
} TR_READER;

/** One output row of TrAggregate: a process (pid + create time) over a time range. */
typedef struct _TR_AGGREGATE
{
    uint32_t ProcessId;
    uint32_t NameIndex;
    int64_t CreateTime;
    double AvgCpuPercent; // of the whole system, averaged over all samples in the range (absent = 0)
    double PeakCpuPercent;
    uint32_t AvgPrivateKB; // over the samples it appears in
    uint32_t PeakPrivateKB;
    uint32_t PeakWorkingSetKB;
    uint32_t Samples; // samples it appears in
} TR_AGGREGATE;

#ifdef __cplusplus
extern "C" {
#endif

uint32_t TrEncodeHeader(uint8_t *Out, uint32_t Capacity);
uint32_t TrEncodeString(uint8_t *Out, uint32_t Capacity, const uint16_t *Text, uint32_t Chars);
uint32_t TrEncodeSampleSize(uint32_t Count);
uint32_t TrEncodeSample(uint8_t *Out, uint32_t Capacity, const TR_SYSTEM *System, const TR_PROCESS *Processes, uint32_t Count);

/**
 * Parses a whole file image. Returns -1 when the header is wrong, otherwise the number of records read.
 * ValidBytes receives the offset after the last complete record (where an append may continue).
 */
int TrParse(const uint8_t *Data, size_t Size, const TR_READER *Reader, size_t *ValidBytes);

/**
 * Thinning: Times ascending (FILETIME). Everything younger than 6 hours is kept; 6 to 48 hours keeps the first
 * sample of every 5 minutes; older than 48 hours the first of every 30 minutes. Keep[i] is set to 1 or 0.
 * Idempotent. Returns the number kept.
 */
uint32_t TrThin(const int64_t *Times, uint32_t Count, int64_t Now, uint8_t *Keep);

/**
 * Per-process figures over the samples whose time is in [From, To]. Samples is an array of Count pointers
 * ascending by time. Rows are returned malloc'ed, sorted by AvgCpuPercent descending (Rank = 0) or by
 * PeakPrivateKB descending (Rank = 1). Free with TrFreeAggregate. Returns the number of rows; SamplesInRange
 * receives how many samples fell in the range.
 */
uint32_t TrAggregate(
    TR_SAMPLE *const *Samples,
    uint32_t Count,
    int64_t From,
    int64_t To,
    int Rank,
    TR_AGGREGATE **Rows,
    uint32_t *SamplesInRange
    );

void TrFreeAggregate(TR_AGGREGATE *Rows);

#ifdef __cplusplus
}
#endif

#endif
