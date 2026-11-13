/*
 * TaskManagerPlus ResourceTrends: leak detection logic.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 */

#ifndef _LK_LEAK_H_
#define _LK_LEAK_H_

#include <stdint.h>

typedef struct _LK_POINT
{
    int64_t Time; // any monotonic unit; the caller passes seconds
    uint64_t ValueKB; // private bytes / commit in KB
} LK_POINT;

typedef struct _LK_CONFIG
{
    int64_t WindowSeconds; // look at the last N seconds of points
    uint64_t MinGrowthKB; // growth over the window needed to flag
    unsigned MinGrowthPercent; // ... and at least this much relative to the value at the window start
    unsigned MinRisePercent; // share of steps that must not go down (default 80)
    unsigned MinPoints; // fewest points inside the window (default 6)
} LK_CONFIG;

typedef struct _LK_RESULT
{
    int Suspect;
    unsigned Points; // points inside the window
    int64_t SpanSeconds; // time covered by those points
    uint64_t GrowthKB; // last - first (0 when it shrank)
    unsigned RisePercent; // non-decreasing steps / steps
    unsigned UpPercent; // strictly increasing steps / steps
    double RateKBPerMinute; // least-squares slope
} LK_RESULT;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Decides whether a series (ascending by Time) shows steady growth in its last WindowSeconds. The window must be
 * at least 60 percent covered by data, and growth has to be both large (MinGrowthKB, MinGrowthPercent) and steady
 * (few downward steps, many upward ones), so a one-time jump after loading something, a saw-tooth (garbage
 * collector) or noise do not flag.
 */
void LkEvaluate(
    const LK_POINT *Points,
    unsigned Count,
    const LK_CONFIG *Config,
    LK_RESULT *Result
    );

#ifdef __cplusplus
}
#endif

#endif
