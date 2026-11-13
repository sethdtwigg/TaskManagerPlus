/*
 * TaskManagerPlus EfficiencyAdvisor: statistics and ranking. See advisor.h.
 */

#include "advisor.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void AdDefaultConfig(AD_CONFIG *Config)
{
    Config->MinBackgroundSeconds = 60.0;
    Config->HalfLifeSeconds = 300.0;
    Config->CpuThresholdPercent = 1.0;
    Config->MemoryThresholdMB = 400.0;
    Config->IoThresholdMBps = 1.0;
    Config->MinScore = 1.0;
}

static int AdFinite(double v)
{
    return v == v && v > -1e300 && v < 1e300;
}

void AdUpdate(
    AD_STATS *Stats,
    const AD_CONFIG *Config,
    double Seconds,
    int InBackground,
    double CpuPercent,
    double IoBytesPerSecond,
    uint64_t WorkingSetKB,
    uint64_t PrivateKB
    )
{
    if (!AdFinite(Seconds) || Seconds <= 0.0 || !AdFinite(CpuPercent) || !AdFinite(IoBytesPerSecond))
        return;

    if (CpuPercent < 0.0)
        CpuPercent = 0.0;

    if (CpuPercent > 100.0)
        CpuPercent = 100.0;

    if (IoBytesPerSecond < 0.0)
        IoBytesPerSecond = 0.0;

    Stats->TotalSeconds += Seconds;
    Stats->WorkingSetKB = WorkingSetKB;
    Stats->PrivateKB = PrivateKB;
    Stats->InBackgroundNow = InBackground ? 1 : 0;

    if (!InBackground)
        return; // foreground time counts for nothing: neither adds to nor decays the background figures

    if (Stats->BackgroundSeconds <= 0.0)
    {
        Stats->AvgCpuPercent = CpuPercent;
        Stats->AvgIoBytesPerSecond = IoBytesPerSecond;
    }
    else
    {
        double halfLife = Config->HalfLifeSeconds > 1.0 ? Config->HalfLifeSeconds : 1.0;
        double alpha = 1.0 - pow(0.5, Seconds / halfLife);

        Stats->AvgCpuPercent += alpha * (CpuPercent - Stats->AvgCpuPercent);
        Stats->AvgIoBytesPerSecond += alpha * (IoBytesPerSecond - Stats->AvgIoBytesPerSecond);
    }

    Stats->BackgroundSeconds += Seconds;
}

double AdScore(const AD_STATS *Stats, const AD_CONFIG *Config, AD_REASON *Reason)
{
    double cpu;
    double mem;
    double io;
    double score;
    double memWeighted;

    if (Reason)
        *Reason = AdReasonNone;

    if (!Stats->InBackgroundNow || Stats->BackgroundSeconds < Config->MinBackgroundSeconds)
        return 0.0;

    cpu = Config->CpuThresholdPercent > 0.0 ? Stats->AvgCpuPercent / Config->CpuThresholdPercent : 0.0;
    mem = Config->MemoryThresholdMB > 0.0 ? ((double)Stats->WorkingSetKB / 1024.0) / Config->MemoryThresholdMB : 0.0;
    io = Config->IoThresholdMBps > 0.0 ? (Stats->AvgIoBytesPerSecond / (1024.0 * 1024.0)) / Config->IoThresholdMBps : 0.0;
    memWeighted = 0.5 * mem;
    score = cpu + memWeighted + io;

    if (score < Config->MinScore)
        return 0.0;

    if (Reason)
    {
        if (cpu >= memWeighted && cpu >= io)
            *Reason = AdReasonCpu;
        else if (io >= memWeighted)
            *Reason = AdReasonDisk;
        else
            *Reason = AdReasonMemory;
    }

    return score;
}

static const AD_CANDIDATE *AdSortBase = NULL;

static int AdCompare(const void *A, const void *B)
{
    const AD_CANDIDATE *a = &AdSortBase[*(const uint32_t *)A];
    const AD_CANDIDATE *b = &AdSortBase[*(const uint32_t *)B];

    if (a->Score != b->Score)
        return a->Score < b->Score ? 1 : -1;

    if (a->ProcessId != b->ProcessId)
        return a->ProcessId < b->ProcessId ? -1 : 1;

    return 0;
}

uint32_t AdRank(AD_CANDIDATE *Candidates, uint32_t Count, const AD_CONFIG *Config, uint32_t *Order)
{
    uint32_t i;
    uint32_t n = 0;

    for (i = 0; i < Count; i++)
    {
        Candidates[i].Score = 0.0;
        Candidates[i].Reason = AdReasonNone;

        if (Candidates[i].Excluded)
            continue;

        Candidates[i].Score = AdScore(&Candidates[i].Stats, Config, &Candidates[i].Reason);

        if (Candidates[i].Score > 0.0)
            Order[n++] = i;
    }

    AdSortBase = Candidates;
    qsort(Order, n, sizeof(uint32_t), AdCompare);
    AdSortBase = NULL;
    return n;
}

int AdIsCriticalName(const wchar_t *Name)
{
    static const wchar_t *names[] =
    {
        L"System", L"Registry", L"Memory Compression", L"Secure System", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe", L"fontdrvhost.exe", L"dwm.exe",
        L"explorer.exe", L"sihost.exe", L"ctfmon.exe", L"taskhostw.exe", L"audiodg.exe", L"spoolsv.exe", L"dllhost.exe",
        L"conhost.exe", L"RuntimeBroker.exe", L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe",
        L"SearchHost.exe", L"TextInputHost.exe", L"MsMpEng.exe", L"NisSrv.exe", L"SecurityHealthService.exe",
        L"MsSense.exe", L"Idle", L"System Idle Process", L"Interrupts", L"DPCs"
    };
    size_t i;

    if (!Name || !*Name)
        return 1; // unknown name: do not touch

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (!_wcsicmp(Name, names[i]))
            return 1;
    }

    return 0;
}

int AdIsIgnored(const wchar_t *IgnoreList, const wchar_t *Name)
{
    const wchar_t *p = IgnoreList;
    size_t nameLength;

    if (!IgnoreList || !Name || !*Name)
        return 0;

    nameLength = wcslen(Name);

    while (*p)
    {
        const wchar_t *end = p;
        size_t length;

        while (*end && *end != L'\n' && *end != L'\r')
            end++;

        length = (size_t)(end - p);

        if (length == nameLength && !_wcsnicmp(p, Name, length))
            return 1;

        while (*end == L'\n' || *end == L'\r')
            end++;

        p = end;
    }

    return 0;
}
