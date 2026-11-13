/*
 * TaskManagerPlus ResourceTrends: leak detection logic (see leak.h).
 */

#include "leak.h"
#include <string.h>

void LkEvaluate(
    const LK_POINT *Points,
    unsigned Count,
    const LK_CONFIG *Config,
    LK_RESULT *Result
    )
{
    unsigned first;
    unsigned n;
    unsigned i;
    unsigned noDown = 0;
    unsigned up = 0;
    int64_t cutoff;
    double sumX = 0;
    double sumY = 0;
    double sumXY = 0;
    double sumXX = 0;
    double denominator;
    uint64_t firstValue;
    uint64_t lastValue;

    memset(Result, 0, sizeof(LK_RESULT));

    if (Count < 2)
        return;

    cutoff = Points[Count - 1].Time - Config->WindowSeconds;

    for (first = 0; first < Count && Points[first].Time < cutoff; first++)
        ;

    n = Count - first;
    Result->Points = n;

    if (n < 2)
        return;

    Result->SpanSeconds = Points[Count - 1].Time - Points[first].Time;

    if (n < Config->MinPoints || Result->SpanSeconds * 10 < Config->WindowSeconds * 6)
        return;

    for (i = first + 1; i < Count; i++)
    {
        uint64_t previous = Points[i - 1].ValueKB;
        uint64_t current = Points[i].ValueKB;
        uint64_t tolerance = previous / 500; // 0.2 percent: ignore jitter

        if (current + tolerance >= previous)
            noDown++;

        if (current > previous + tolerance)
            up++;
    }

    Result->RisePercent = noDown * 100 / (n - 1);
    Result->UpPercent = up * 100 / (n - 1);

    firstValue = Points[first].ValueKB;
    lastValue = Points[Count - 1].ValueKB;
    Result->GrowthKB = lastValue > firstValue ? lastValue - firstValue : 0;

    for (i = first; i < Count; i++)
    {
        double x = (double)(Points[i].Time - Points[first].Time);
        double y = (double)Points[i].ValueKB;

        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }

    denominator = (double)n * sumXX - sumX * sumX;

    if (denominator > 0)
        Result->RateKBPerMinute = ((double)n * sumXY - sumX * sumY) / denominator * 60.0;

    if (Result->GrowthKB >= Config->MinGrowthKB &&
        Result->GrowthKB * 100 >= firstValue * Config->MinGrowthPercent &&
        Result->RisePercent >= Config->MinRisePercent &&
        Result->UpPercent >= 50 &&
        Result->RateKBPerMinute > 0)
    {
        Result->Suspect = 1;
    }
}
