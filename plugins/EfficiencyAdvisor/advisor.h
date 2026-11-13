/*
 * TaskManagerPlus EfficiencyAdvisor: the background-offender statistics and ranking.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 *
 * For every process the plugin feeds one sample per refresh. Only samples taken while the process is NOT part of the
 * foreground application count towards its "background" figures: average CPU (percent of the whole system) and disk
 * rate are exponential moving averages over background time only (half-life AD_CONFIG.HalfLifeSeconds), so a process that
 * calms down or that you start using drops off by itself. Memory is the latest working set.
 *
 * Score = cpu / CpuThreshold + 0.5 * memory / MemoryThreshold + disk / IoThreshold, and a process is listed when it has
 * been in the background for at least MinBackgroundSeconds, is in the background right now, and scores at least MinScore.
 * Defaults: 1 % of the whole system CPU, 400 MB, 1 MB/s, 60 s, score 1.
 */

#ifndef _AD_ADVISOR_H_
#define _AD_ADVISOR_H_

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

typedef struct _AD_CONFIG
{
    double MinBackgroundSeconds;
    double HalfLifeSeconds;
    double CpuThresholdPercent; // of the whole system
    double MemoryThresholdMB;
    double IoThresholdMBps;
    double MinScore;
} AD_CONFIG;

typedef struct _AD_STATS
{
    double TotalSeconds;
    double BackgroundSeconds; // time spent in the background (samples that counted)
    double AvgCpuPercent; // background average, percent of the whole system
    double AvgIoBytesPerSecond; // background average of read + write bytes per second
    uint64_t WorkingSetKB; // latest
    uint64_t PrivateKB; // latest
    int InBackgroundNow; // the latest sample was a background one
} AD_STATS;

typedef enum _AD_REASON
{
    AdReasonNone = 0,
    AdReasonCpu,
    AdReasonMemory,
    AdReasonDisk
} AD_REASON;

typedef struct _AD_CANDIDATE
{
    AD_STATS Stats;
    int Excluded; // critical / system / protected / ignored: never listed
    uint32_t ProcessId; // tie breaker
    // output of AdRank
    double Score;
    AD_REASON Reason;
} AD_CANDIDATE;

#ifdef __cplusplus
extern "C" {
#endif

void AdDefaultConfig(AD_CONFIG *Config);

/** One sample of Seconds length. Samples with a non-positive length or non-finite values are ignored. */
void AdUpdate(
    AD_STATS *Stats,
    const AD_CONFIG *Config,
    double Seconds,
    int InBackground,
    double CpuPercent,
    double IoBytesPerSecond,
    uint64_t WorkingSetKB,
    uint64_t PrivateKB
    );

/** The score (0 when the process does not qualify: too little background time, in the foreground now, or below MinScore). */
double AdScore(const AD_STATS *Stats, const AD_CONFIG *Config, AD_REASON *Reason);

/**
 * Scores the candidates (Score/Reason are filled in), drops excluded and unqualified ones and writes the indexes of the
 * rest, best first (ties by process id), into Order (capacity Count). Returns how many.
 */
uint32_t AdRank(AD_CANDIDATE *Candidates, uint32_t Count, const AD_CONFIG *Config, uint32_t *Order);

/** 1 for image names the advisor never touches (System, csrss, svchost, dwm, explorer ...). Case-insensitive. */
int AdIsCriticalName(const wchar_t *Name);

/** 1 when Name (or the whole text) matches an entry of a newline separated, case-insensitive ignore list. */
int AdIsIgnored(const wchar_t *IgnoreList, const wchar_t *Name);

#ifdef __cplusplus
}
#endif

#endif
