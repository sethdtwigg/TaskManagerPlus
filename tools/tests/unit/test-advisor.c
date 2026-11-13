/*
 * Unit tests for the Efficiency advisor (Phase 7 item 5): foreground time does not count, sustained use qualifies and a
 * short burst does not, decay after calming down, scoring and the reason, ranking order and ties, exclusions, ignore list.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../../plugins/EfficiencyAdvisor/advisor.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void Feed(AD_STATS *s, const AD_CONFIG *c, int seconds, int bg, double cpu, double io, uint64_t wsKB)
{
    int i;

    for (i = 0; i < seconds; i++)
        AdUpdate(s, c, 1.0, bg, cpu, io, wsKB, wsKB);
}

static void TestStats(void)
{
    AD_CONFIG c;
    AD_STATS s;
    AD_REASON r;

    AdDefaultConfig(&c);
    memset(&s, 0, sizeof(s));

    // foreground time counts for nothing
    Feed(&s, &c, 600, 0, 50.0, 0, 100 * 1024);
    CHECK(s.BackgroundSeconds == 0.0 && s.AvgCpuPercent == 0.0 && s.TotalSeconds == 600.0);
    CHECK(AdScore(&s, &c, &r) == 0.0 && r == AdReasonNone);

    // not long enough in the background
    Feed(&s, &c, 59, 1, 5.0, 0, 100 * 1024);
    CHECK(AdScore(&s, &c, &r) == 0.0);
    Feed(&s, &c, 1, 1, 5.0, 0, 100 * 1024);
    CHECK(s.BackgroundSeconds == 60.0 && fabs(s.AvgCpuPercent - 5.0) < 1e-9);
    CHECK(AdScore(&s, &c, &r) > 0.0 && r == AdReasonCpu);

    // switching to it (foreground now) hides it without losing the history
    AdUpdate(&s, &c, 1.0, 0, 90.0, 0, 100 * 1024, 100 * 1024);
    CHECK(AdScore(&s, &c, &r) == 0.0 && s.BackgroundSeconds == 60.0 && fabs(s.AvgCpuPercent - 5.0) < 1e-9);
    AdUpdate(&s, &c, 1.0, 1, 5.0, 0, 100 * 1024, 100 * 1024);
    CHECK(AdScore(&s, &c, &r) > 0.0);

    // a short burst: 20 s at 80 % in a process that mostly idles does not reach the threshold
    memset(&s, 0, sizeof(s));
    Feed(&s, &c, 280, 1, 0.0, 0, 20 * 1024);
    Feed(&s, &c, 20, 1, 80.0, 0, 20 * 1024);
    CHECK(s.AvgCpuPercent > 1.0); // still averaging in the burst (EMA)
    {
        // ... and it fades: 20 minutes of calm brings it under the 1 % threshold
        double before = s.AvgCpuPercent;

        Feed(&s, &c, 1200, 1, 0.0, 0, 20 * 1024);
        CHECK(s.AvgCpuPercent < before / 10.0 && AdScore(&s, &c, &r) == 0.0);
    }

    // a sustained moderate user passes
    memset(&s, 0, sizeof(s));
    Feed(&s, &c, 300, 1, 2.0, 0, 50 * 1024);
    CHECK(fabs(s.AvgCpuPercent - 2.0) < 1e-6 && AdScore(&s, &c, &r) >= 2.0);

    // memory alone: 1 GB resident is 0.5 * 2.56 = 1.28 >= 1; 300 MB is not
    memset(&s, 0, sizeof(s));
    Feed(&s, &c, 120, 1, 0.0, 0, 1024 * 1024);
    CHECK(AdScore(&s, &c, &r) > 1.0 && r == AdReasonMemory);
    memset(&s, 0, sizeof(s));
    Feed(&s, &c, 120, 1, 0.0, 0, 300 * 1024);
    CHECK(AdScore(&s, &c, &r) == 0.0);

    // disk alone: 4 MB/s
    memset(&s, 0, sizeof(s));
    Feed(&s, &c, 120, 1, 0.0, 4.0 * 1024 * 1024, 10 * 1024);
    CHECK(AdScore(&s, &c, &r) >= 4.0 && r == AdReasonDisk);

    // bad input is ignored
    memset(&s, 0, sizeof(s));
    AdUpdate(&s, &c, 0.0, 1, 5.0, 0, 1, 1);
    AdUpdate(&s, &c, -3.0, 1, 5.0, 0, 1, 1);
    AdUpdate(&s, &c, 1.0, 1, NAN, 0, 1, 1);
    AdUpdate(&s, &c, 1.0, 1, 5.0, INFINITY, 1, 1);
    CHECK(s.TotalSeconds == 0.0 && s.BackgroundSeconds == 0.0);
    AdUpdate(&s, &c, 1.0, 1, 500.0, -5.0, 1, 1); // clamped
    CHECK(s.AvgCpuPercent == 100.0 && s.AvgIoBytesPerSecond == 0.0);

    // EMA is time weighted: one 300 s sample moves it half way (half-life 300 s)
    memset(&s, 0, sizeof(s));
    AdUpdate(&s, &c, 1.0, 1, 0.0, 0, 1, 1);
    AdUpdate(&s, &c, 300.0, 1, 10.0, 0, 1, 1);
    CHECK(fabs(s.AvgCpuPercent - 5.0) < 1e-9);

    // a zero threshold must not divide by zero
    c.CpuThresholdPercent = 0.0;
    c.MemoryThresholdMB = 0.0;
    c.IoThresholdMBps = 0.0;
    memset(&s, 0, sizeof(s));
    Feed(&s, &c, 120, 1, 50.0, 1e9, 1 << 30);
    CHECK(AdScore(&s, &c, &r) == 0.0);
}

static void TestRanking(void)
{
    AD_CONFIG c;
    AD_CANDIDATE cand[6];
    uint32_t order[6];
    uint32_t n;

    AdDefaultConfig(&c);
    memset(cand, 0, sizeof(cand));

    // 0: heavy sustained CPU, 1: moderate, 2: heavy but in the foreground now, 3: heavy but excluded,
    // 4: heavy but only 10 s of background, 5: same score as 1 (tie by pid)
    cand[0].ProcessId = 100; Feed(&cand[0].Stats, &c, 200, 1, 10.0, 0, 100 * 1024);
    cand[1].ProcessId = 300; Feed(&cand[1].Stats, &c, 200, 1, 3.0, 0, 100 * 1024);
    cand[2].ProcessId = 200; Feed(&cand[2].Stats, &c, 200, 1, 40.0, 0, 100 * 1024); AdUpdate(&cand[2].Stats, &c, 1.0, 0, 40.0, 0, 1, 1);
    cand[3].ProcessId = 400; cand[3].Excluded = 1; Feed(&cand[3].Stats, &c, 200, 1, 40.0, 0, 100 * 1024);
    cand[4].ProcessId = 500; Feed(&cand[4].Stats, &c, 10, 1, 40.0, 0, 100 * 1024);
    cand[5].ProcessId = 250; Feed(&cand[5].Stats, &c, 200, 1, 3.0, 0, 100 * 1024);

    n = AdRank(cand, 6, &c, order);
    CHECK(n == 3);
    CHECK(order[0] == 0 && order[1] == 5 && order[2] == 1); // 10 %, then the tie (pid 250 before 300)
    CHECK(cand[0].Score > cand[5].Score && cand[5].Score == cand[1].Score);
    CHECK(cand[0].Reason == AdReasonCpu);
    CHECK(cand[2].Score == 0.0 && cand[3].Score == 0.0 && cand[4].Score == 0.0);

    // nothing qualifies
    memset(cand, 0, sizeof(cand));
    CHECK(AdRank(cand, 6, &c, order) == 0);
    CHECK(AdRank(cand, 0, &c, order) == 0);
}

static void TestNames(void)
{
    CHECK(AdIsCriticalName(L"csrss.exe") && AdIsCriticalName(L"CSRSS.EXE") && AdIsCriticalName(L"System"));
    CHECK(AdIsCriticalName(L"svchost.exe") && AdIsCriticalName(L"dwm.exe") && AdIsCriticalName(L"explorer.exe") && AdIsCriticalName(L"lsass.exe"));
    CHECK(AdIsCriticalName(L"Registry") && AdIsCriticalName(L"Memory Compression") && AdIsCriticalName(L"MsMpEng.exe"));
    CHECK(!AdIsCriticalName(L"chrome.exe") && !AdIsCriticalName(L"OneDrive.exe") && !AdIsCriticalName(L"code.exe"));
    CHECK(AdIsCriticalName(L"") && AdIsCriticalName(NULL)); // unknown names are never touched

    CHECK(AdIsIgnored(L"chrome.exe\nOneDrive.exe\n", L"onedrive.exe"));
    CHECK(AdIsIgnored(L"chrome.exe\r\nOneDrive.exe", L"OneDrive.exe"));
    CHECK(!AdIsIgnored(L"chrome.exe\nOneDrive.exe\n", L"one"));
    CHECK(!AdIsIgnored(L"chrome.exe\nOneDrive.exe\n", L"chrome.exe2"));
    CHECK(!AdIsIgnored(L"", L"chrome.exe") && !AdIsIgnored(NULL, L"chrome.exe") && !AdIsIgnored(L"a\n", L"") && !AdIsIgnored(L"a\n", NULL));
}

int main(void)
{
    TestStats();
    TestRanking();
    TestNames();
    printf("advisor: %d checks, %d failed\n", Checks, Failures);
    return Failures ? 1 : 0;
}
