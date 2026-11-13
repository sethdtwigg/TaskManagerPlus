/*
 * Unit tests for the pure-C logic of the QuickActions and ResourceTrends plugins:
 * fuzzy matcher, trend file format (round trip, truncation, bad header), thinning, aggregation, leak detection.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe, no System Informer headers needed).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../../plugins/QuickActions/fuzzy.h"
#include "../../../plugins/ResourceTrends/leak.h"
#include "../../../plugins/ResourceTrends/trendfmt.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// ---------------------------------------------------------------- fuzzy

static void TestFuzzy(void)
{
    long exact = QaFuzzyScore(L"notepad", L"notepad");
    long prefix = QaFuzzyScore(L"note", L"notepad.exe");
    long scattered = QaFuzzyScore(L"note", L"nothing to explore");
    long consecutive = QaFuzzyScore(L"chr", L"chrome.exe");
    long wordStart = QaFuzzyScore(L"se", L"Search Enhancement");
    long mid = QaFuzzyScore(L"se", L"possessive");

    CHECK(QaFuzzyScore(L"", L"anything") == 0);
    CHECK(QaFuzzyScore(NULL, L"anything") == 0);
    CHECK(QaFuzzyScore(L"   ", L"anything") == 0);
    CHECK(QaFuzzyScore(L"zzz", L"notepad") == QA_NO_MATCH);
    CHECK(QaFuzzyScore(L"notepadx", L"notepad") == QA_NO_MATCH); // pattern longer than text
    CHECK(QaFuzzyScore(L"dap", L"notepad") == QA_NO_MATCH); // wrong order is not a subsequence
    CHECK(QaFuzzyScore(L"NOTE", L"notepad") != QA_NO_MATCH); // case-insensitive
    CHECK(exact > prefix);
    CHECK(prefix > scattered);
    CHECK(consecutive > QaFuzzyScore(L"chr", L"c h r o m e"));
    CHECK(wordStart > mid);
    // multiple words: all must match, order free
    CHECK(QaFuzzyScore(L"end note", L"End task notepad.exe 1234") != QA_NO_MATCH);
    CHECK(QaFuzzyScore(L"note end", L"End task notepad.exe 1234") != QA_NO_MATCH);
    CHECK(QaFuzzyScore(L"end zzz", L"End task notepad.exe 1234") == QA_NO_MATCH);
    // a better match ranks higher
    CHECK(QaFuzzyScore(L"chrome", L"chrome.exe") > QaFuzzyScore(L"chrome", L"Google Chrome Helper"));
    // characters scattered across far more text than the word is long do not match
    CHECK(QaFuzzyScore(L"chrome", L"End process tree kill terminate tree children vmcompute.exe process 12248") == QA_NO_MATCH);
    CHECK(QaFuzzyScore(L"chrome", L"Stop service Razer Chroma SDK Server") != QA_NO_MATCH);
    CHECK(QaFuzzyScore(L"ntp", L"notepad") != QA_NO_MATCH); // short gaps inside one word are fine
    // pids match as text
    CHECK(QaFuzzyScore(L"1234", L"notepad.exe 1234") != QA_NO_MATCH);
    // long text is clipped, not crashed
    {
        wchar_t big[600];
        int i;

        for (i = 0; i < 599; i++)
            big[i] = L'a';

        big[599] = 0;
        CHECK(QaFuzzyScore(L"aaa", big) != QA_NO_MATCH);
    }
}

// ---------------------------------------------------------------- trend format

typedef struct _CAPTURE
{
    int Strings;
    int Samples;
    uint32_t LastCount;
    TR_SYSTEM LastSystem;
    TR_PROCESS LastFirst;
    wchar_t FirstString[64];
} CAPTURE;

static int OnString(void *Context, const uint8_t *Text, uint32_t Chars)
{
    CAPTURE *c = Context;

    if (c->Strings == 0 && Chars < 63)
    {
        memcpy(c->FirstString, Text, Chars * 2);
        c->FirstString[Chars] = 0;
    }

    c->Strings++;
    return 1;
}

static int OnSample(void *Context, const TR_SYSTEM *System, const TR_PROCESS *Processes, uint32_t Count)
{
    CAPTURE *c = Context;

    c->Samples++;
    c->LastCount = Count;
    c->LastSystem = *System;

    if (Count)
        c->LastFirst = Processes[0];

    return 1;
}

static uint32_t AppendSample(uint8_t *file, uint32_t at, int64_t time, uint32_t pidBase, uint32_t count)
{
    TR_SYSTEM s;
    TR_PROCESS p[8];
    uint32_t i;

    memset(&s, 0, sizeof(s));
    s.Time = time;
    s.CpuX100 = 4321;
    s.KernelX100 = 1000;
    s.PhysicalUsedMB = 8000;
    s.PhysicalTotalMB = 16000;
    s.CommitUsedMB = 12000;
    s.CommitLimitMB = 40000;

    for (i = 0; i < count; i++)
    {
        memset(&p[i], 0, sizeof(TR_PROCESS));
        p[i].ProcessId = pidBase + i;
        p[i].NameIndex = i;
        p[i].CreateTime = 132000000000000000LL + i;
        p[i].PrivateKB = 1000 * (i + 1);
        p[i].WorkingSetKB = 500 * (i + 1);
        p[i].CpuX100 = (uint16_t)(100 * (i + 1));
        p[i].Flags = 0;
    }

    return TrEncodeSample(file + at, 4096 - at, &s, p, count);
}

static void TestFormat(void)
{
    static uint8_t file[4096];
    uint32_t at = 0;
    uint32_t n;
    uint16_t name[] = { 'a', 'b', 0x20AC }; // includes a non-ASCII character
    TR_READER reader;
    CAPTURE cap;
    size_t valid;
    int records;

    memset(&reader, 0, sizeof(reader));
    reader.String = OnString;
    reader.Sample = OnSample;

    n = TrEncodeHeader(file, sizeof(file)); CHECK(n == 16); at += n;
    n = TrEncodeString(file + at, sizeof(file) - at, name, 3); CHECK(n == 8 + 6); at += n;
    n = AppendSample(file, at, 1000, 100, 3); CHECK(n == TrEncodeSampleSize(3)); CHECK(n == 8 + 40 + 3 * 28); at += n;
    n = AppendSample(file, at, 2000, 200, 2); at += n;
    CHECK(TrEncodeSample(file, 10, &cap.LastSystem, NULL, 0) == 0); // too small a buffer

    // byte-exact layout of the head so the documented format cannot drift
    CHECK(file[0] == 0x54 && file[1] == 0x4D && file[2] == 0x54 && file[3] == 0x52);
    CHECK(file[4] == 1 && file[5] == 0 && file[6] == 0 && file[7] == 0);
    CHECK(file[16] == 1 && file[20] == 6); // string record type 1, 6 bytes
    CHECK(file[16 + 14] == 2); // sample record type 2

    memset(&cap, 0, sizeof(cap));
    reader.Context = &cap;
    records = TrParse(file, at, &reader, &valid);
    CHECK(records == 3);
    CHECK(valid == at);
    CHECK(cap.Strings == 1 && cap.Samples == 2);
    CHECK(wcscmp(cap.FirstString, L"ab\x20AC") == 0);
    CHECK(cap.LastCount == 2);
    CHECK(cap.LastSystem.Time == 2000 && cap.LastSystem.CpuX100 == 4321 && cap.LastSystem.CommitLimitMB == 40000);
    CHECK(cap.LastFirst.ProcessId == 200 && cap.LastFirst.PrivateKB == 1000 && cap.LastFirst.CpuX100 == 100);
    CHECK(cap.LastFirst.CreateTime == 132000000000000000LL);

    // truncated tail: every cut inside the last record leaves the first two records intact
    {
        size_t cut;
        size_t lastStart = at - TrEncodeSampleSize(2);

        for (cut = lastStart; cut < at; cut++)
        {
            memset(&cap, 0, sizeof(cap));
            records = TrParse(file, cut, &reader, &valid);
            CHECK(records == 2 && valid == lastStart && cap.Samples == 1);
        }
    }

    // bad header
    {
        uint8_t bad[32];

        memcpy(bad, file, sizeof(bad));
        bad[0] = 0;
        CHECK(TrParse(bad, sizeof(bad), &reader, &valid) == -1);
        memcpy(bad, file, sizeof(bad));
        bad[4] = 9; // other version
        CHECK(TrParse(bad, sizeof(bad), &reader, &valid) == -1);
        CHECK(TrParse(file, 7, &reader, &valid) == -1);
    }

    // unknown record type and a malformed sample (count disagrees with length) are skipped
    {
        static uint8_t f2[4096];
        uint32_t k = 0;

        k += TrEncodeHeader(f2, sizeof(f2));
        f2[k] = 99; f2[k + 4] = 3; k += 8 + 3; // unknown type, 3 payload bytes
        {
            uint32_t s = AppendSample(f2, k, 5000, 1, 2);

            f2[k + 8 + 32] = 7; // claim 7 processes in a record that holds 2
            k += s;
        }
        k += AppendSample(f2, k, 6000, 10, 1);
        memset(&cap, 0, sizeof(cap));
        records = TrParse(f2, k, &reader, &valid);
        CHECK(records == 3 && cap.Samples == 1 && cap.LastSystem.Time == 6000 && valid == k);
    }
}

// ---------------------------------------------------------------- thinning

static void TestThin(void)
{
    enum { N = 24 * 360 + 1 }; // 24 hours every 10 s
    static int64_t times[N];
    static uint8_t keep[N];
    static uint8_t keep2[N];
    int64_t now = 140000LL * 86400LL * TR_FILETIME_SECOND; // far from zero, not on a bucket edge
    uint32_t i;
    uint32_t kept;
    uint32_t recent = 0;

    now += 123 * TR_FILETIME_SECOND;

    for (i = 0; i < N; i++)
        times[i] = now - (int64_t)(N - 1 - i) * 10 * TR_FILETIME_SECOND;

    kept = TrThin(times, N, now, keep);

    for (i = 0; i < N; i++)
    {
        if (now - times[i] < 6 * TR_FILETIME_HOUR)
        {
            CHECK(keep[i] == 1); // the newest 6 hours are untouched
            recent++;
        }
    }

    CHECK(recent == 6 * 360);
    // 6..24 hours: 18 hours of 5-minute buckets = 216 (+ at most 1 for the bucket edge)
    CHECK(kept >= recent + 216 && kept <= recent + 218);
    CHECK(kept < N / 2);

    // idempotent: thinning the survivors again with the same clock keeps them all
    {
        int64_t t2[N];
        uint32_t m = 0;

        for (i = 0; i < N; i++)
            if (keep[i])
                t2[m++] = times[i];

        CHECK(TrThin(t2, m, now, keep2) == m);
    }

    // older than 48 hours: one per 30 minutes
    {
        int64_t t3[400];
        uint8_t k3[400];
        uint32_t j;

        for (j = 0; j < 400; j++)
            t3[j] = now - 100 * TR_FILETIME_HOUR + (int64_t)j * 60 * TR_FILETIME_SECOND; // 1-minute samples over 6.7 hours

        CHECK(TrThin(t3, 400, now, k3) <= 400 / 30 + 2);
    }
}

// ---------------------------------------------------------------- aggregation

static TR_SAMPLE *MakeSample(int64_t time, uint32_t count, const TR_PROCESS *p)
{
    TR_SAMPLE *s = calloc(1, sizeof(TR_SAMPLE));

    s->System.Time = time;
    s->Count = count;
    s->Processes = malloc(count * sizeof(TR_PROCESS));
    memcpy(s->Processes, p, count * sizeof(TR_PROCESS));
    return s;
}

static void TestAggregate(void)
{
    TR_SAMPLE *samples[4];
    TR_PROCESS a[2];
    TR_AGGREGATE *rows;
    uint32_t inRange;
    uint32_t n;
    int i;

    memset(a, 0, sizeof(a));
    // process 10 uses CPU only in the first two samples; process 20 is steady and holds more memory
    a[0].ProcessId = 10; a[0].CreateTime = 1; a[0].CpuX100 = 5000; a[0].PrivateKB = 1000; a[0].WorkingSetKB = 900;
    a[1].ProcessId = 20; a[1].CreateTime = 2; a[1].CpuX100 = 500; a[1].PrivateKB = 90000; a[1].WorkingSetKB = 80000;
    samples[0] = MakeSample(100, 2, a);
    samples[1] = MakeSample(200, 2, a);
    a[0].CpuX100 = 0; a[0].PrivateKB = 1500;
    samples[2] = MakeSample(300, 2, a);
    samples[3] = MakeSample(400, 2, a);

    // range [100,200]: two samples, process 10 averages 50 percent
    n = TrAggregate(samples, 4, 100, 200, 0, &rows, &inRange);
    CHECK(n == 2 && inRange == 2);
    CHECK(rows[0].ProcessId == 10 && rows[0].AvgCpuPercent > 49.9 && rows[0].AvgCpuPercent < 50.1);
    CHECK(rows[1].ProcessId == 20 && rows[1].Samples == 2);
    TrFreeAggregate(rows);

    // all four samples: average of 50,50,0,0 = 25
    n = TrAggregate(samples, 4, 0, 1000, 0, &rows, &inRange);
    CHECK(n == 2 && inRange == 4);
    CHECK(rows[0].ProcessId == 10 && rows[0].AvgCpuPercent > 24.9 && rows[0].AvgCpuPercent < 25.1);
    CHECK(rows[0].PeakCpuPercent > 49.9 && rows[0].PeakPrivateKB == 1500);
    CHECK(rows[0].AvgPrivateKB == (1000 + 1000 + 1500 + 1500) / 4);
    TrFreeAggregate(rows);

    // ranking by memory puts the 90 MB process first
    n = TrAggregate(samples, 4, 0, 1000, 1, &rows, &inRange);
    CHECK(n == 2 && rows[0].ProcessId == 20 && rows[0].PeakPrivateKB == 90000);
    TrFreeAggregate(rows);

    // a pid reused with another create time is a different row
    a[0].ProcessId = 20; a[0].CreateTime = 3;
    {
        TR_SAMPLE *reuse = MakeSample(500, 2, a);
        TR_SAMPLE *list[5] = { samples[0], samples[1], samples[2], samples[3], reuse };

        n = TrAggregate(list, 5, 0, 1000, 0, &rows, &inRange);
        CHECK(n == 3);
        TrFreeAggregate(rows);
        free(reuse->Processes);
        free(reuse);
    }

    // empty range
    n = TrAggregate(samples, 4, 5000, 6000, 0, &rows, &inRange);
    CHECK(n == 0 && inRange == 0 && rows == NULL);

    for (i = 0; i < 4; i++)
    {
        free(samples[i]->Processes);
        free(samples[i]);
    }
}

// ---------------------------------------------------------------- leak detection

static void Series(LK_POINT *p, unsigned n, int64_t step, uint64_t start, double perStepKB, int pattern)
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        double v = (double)start + perStepKB * i;

        if (pattern == 1) // saw tooth: grows 10 steps then is freed
            v = (double)start + perStepKB * (i % 10);
        else if (pattern == 2) // one jump in the middle, flat otherwise
            v = (double)start + (i >= n / 2 ? perStepKB * 20 : 0);
        else if (pattern == 3) // noisy but growing a little: +-1% jitter on a flat value
            v = (double)start * (1.0 + ((i * 7) % 5 - 2) * 0.005);
        else if (pattern == 4) // leak with some steps down (80 percent kept)
            v = (double)start + perStepKB * i - ((i % 5 == 4) ? perStepKB * 0.6 : 0);
        else if (pattern == 5) // shrinking
            v = (double)start - perStepKB * i;

        p[i].Time = (int64_t)i * step;
        p[i].ValueKB = (uint64_t)v;
    }
}

static void TestLeak(void)
{
    LK_POINT p[200];
    LK_RESULT r;
    LK_CONFIG cfg;

    cfg.WindowSeconds = 1800;
    cfg.MinGrowthKB = 50 * 1024;
    cfg.MinGrowthPercent = 10;
    cfg.MinRisePercent = 80;
    cfg.MinPoints = 6;

    // steady leak: 31 points one minute apart (30 minutes), +4 MB/min = 120 MB
    Series(p, 31, 60, 200 * 1024, 4096, 0);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 1 && r.Points == 31 && r.RisePercent == 100 && r.UpPercent == 100);
    CHECK(r.GrowthKB == 30 * 4096 && r.RateKBPerMinute > 4000 && r.RateKBPerMinute < 4200);

    // the same leak but only 10 minutes of data against a 30 minute window: not enough coverage yet
    LkEvaluate(p, 11, &cfg, &r);
    CHECK(r.Suspect == 0);

    // far more data than the window: only the last 30 minutes count
    Series(p, 100, 60, 100 * 1024, 4096, 0);
    LkEvaluate(p, 100, &cfg, &r);
    CHECK(r.Suspect == 1 && r.Points == 31);

    // flat
    Series(p, 31, 60, 500 * 1024, 0, 0);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0 && r.GrowthKB == 0);

    // saw tooth (growth then free) is not a leak
    Series(p, 31, 60, 300 * 1024, 8192, 1);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0);

    // one-time jump after loading something
    Series(p, 31, 60, 100 * 1024, 8192, 2);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0);

    // jitter
    Series(p, 31, 60, 500 * 1024, 0, 3);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0);

    // leak with occasional small dips still flags
    Series(p, 31, 60, 200 * 1024, 4096, 4);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 1);

    // shrinking
    Series(p, 31, 60, 900 * 1024, 4096, 5);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0 && r.GrowthKB == 0);

    // growth too small in relative terms: +60 MB on 2 GB is under 10 percent
    Series(p, 31, 60, 2048 * 1024, 2048, 0);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0);

    // too few points, and degenerate inputs
    LkEvaluate(p, 1, &cfg, &r);
    CHECK(r.Suspect == 0);
    LkEvaluate(p, 0, &cfg, &r);
    CHECK(r.Suspect == 0);
    cfg.MinPoints = 40;
    Series(p, 31, 60, 200 * 1024, 4096, 0);
    LkEvaluate(p, 31, &cfg, &r);
    CHECK(r.Suspect == 0);

    // short window used by the hidden-desktop test: 20 s at 1 s samples, +5 MB/s
    cfg.WindowSeconds = 20;
    cfg.MinPoints = 6;
    cfg.MinGrowthKB = 20 * 1024;
    Series(p, 25, 1, 40 * 1024, 5120, 0);
    LkEvaluate(p, 25, &cfg, &r);
    CHECK(r.Suspect == 1);
}

int main(void)
{
    TestFuzzy();
    TestFormat();
    TestThin();
    TestAggregate();
    TestLeak();
    printf("%d checks, %d failed\n", Checks, Failures);
    return Failures ? 1 : 0;
}
