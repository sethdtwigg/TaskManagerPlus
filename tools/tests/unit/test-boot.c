/*
 * Unit tests for StartupManager boot timing (Phase 7 item 7): parsing rendered Diagnostics-Performance events,
 * time parsing, aggregation (newest boot, newest item event), matching to startup entries, impact rating, and
 * graceful handling of empty or malformed input.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../plugins/StartupManager/bootparse.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// Sample events in the shape EvtRender(EvtRenderEventXml) produces (namespace, attributes in single quotes, Qualifiers).
static const wchar_t *Boot100A =
    L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
    L"<Provider Name='Microsoft-Windows-Diagnostics-Performance' Guid='{CFC18EC0-96B1-4EBA-961B-622CAEE05B0A}'/>"
    L"<EventID>100</EventID><Version>2</Version><Level>3</Level><Task>4002</Task>"
    L"<TimeCreated SystemTime='2026-11-10T08:00:11.1234567Z'/><Channel>Microsoft-Windows-Diagnostics-Performance/Operational</Channel>"
    L"</System><EventData>"
    L"<Data Name='BootStartTime'>2026-11-10T07:58:00.000000000Z</Data><Data Name='BootTime'>71234</Data>"
    L"<Data Name='MainPathBootTime'>48000</Data><Data Name='BootPostBootTime'>23234</Data>"
    L"<Data Name='BootKernelInitTime'>4</Data><Data Name='BootDriverInitTime'>3500</Data><Data Name='BootDevicesInitTime'>2100</Data>"
    L"<Data Name='BootSmssInitTime'>8000</Data><Data Name='BootCriticalServicesInitTime'>900</Data>"
    L"<Data Name='BootUserProfileProcessingTime'>5200</Data><Data Name='BootExplorerInitTime'>7000</Data>"
    L"<Data Name='BootNumStartupApps'>14</Data><Data Name='BootIsDegradation'>true</Data>"
    L"<Data Name='OSLoaderDuration'>12000</Data><Data Name='UserLogonWaitDuration'>0</Data></EventData></Event>";

static const wchar_t *Boot100B = // an older boot
    L"<Event><System><EventID Qualifiers='0'>100</EventID><TimeCreated SystemTime='2026-11-01T09:00:00.0000000Z'/></System>"
    L"<EventData><Data Name='BootTime'>55000</Data><Data Name='MainPathBootTime'>40000</Data></EventData></Event>";

static const wchar_t *App101 =
    L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><EventID>101</EventID>"
    L"<TimeCreated SystemTime='2026-11-10T08:00:12.0000000Z'/></System><EventData>"
    L"<Data Name='Name'>OneDrive.exe</Data><Data Name='FriendlyName'>Microsoft OneDrive</Data>"
    L"<Data Name='Path'>C:\\Users\\a\\AppData\\Local\\Microsoft\\OneDrive\\OneDrive.exe</Data>"
    L"<Data Name='TotalTime'>6800</Data><Data Name='DegradationTime'>4100</Data><Data Name='Incidents'>1</Data></EventData></Event>";

static const wchar_t *App101Old = // same item, older boot, smaller
    L"<Event><System><EventID>101</EventID><TimeCreated SystemTime='2026-11-01T09:00:10.0000000Z'/></System><EventData>"
    L"<Data Name='Name'>OneDrive.exe</Data><Data Name='TotalTime'>900</Data><Data Name='DegradationTime'>100</Data></EventData></Event>";

static const wchar_t *Svc103 =
    L"<Event><System><EventID>103</EventID><TimeCreated SystemTime='2026-11-10T08:00:13.5000000Z'/></System><EventData>"
    L"<Data Name='Name'>SysMain</Data><Data Name='FriendlyName'>Superfetch &amp; friends</Data><Data Name='TotalTime'>1500</Data>"
    L"<Data Name='DegradationTime'>600</Data></EventData></Event>";

static const wchar_t *Drv102 =
    L"<Event><System><EventID>102</EventID><TimeCreated SystemTime='2026-11-10T08:00:13Z'/></System><EventData>"
    L"<Data Name='Name'>\\SystemRoot\\system32\\drivers\\foo.sys</Data><Data Name='TotalTime'>2500</Data></EventData></Event>";

static const wchar_t *Entities =
    L"<Event><System><EventID>101</EventID><TimeCreated SystemTime='2026-11-10T08:00:14Z'/></System><EventData>"
    L"<Data Name='Name'>A &lt;b&gt; &quot;q&quot; &apos;s&apos; &#65;&#x42; &bogus; &amp;amp;</Data><Data Name='Path'/>"
    L"<Data Name='TotalTime'>12</Data></EventData></Event>";

static void TestTime(void)
{
    // 1970-01-01T00:00:00Z is 116444736000000000 in FILETIME
    CHECK(BpParseIsoTime(L"1970-01-01T00:00:00.0000000Z") == 116444736000000000LL);
    CHECK(BpParseIsoTime(L"1970-01-01T00:00:01Z") == 116444736000000000LL + 10000000LL);
    CHECK(BpParseIsoTime(L"1970-01-01T00:00:00.5Z") == 116444736000000000LL + 5000000LL);
    CHECK(BpParseIsoTime(L"2000-03-01T00:00:00Z") == 125963424000000000LL); // leap-year boundary
    CHECK(BpParseIsoTime(L"2024-02-29T12:30:45.1234567Z") == 133536834451234567LL);
    CHECK(BpParseIsoTime(L"2026-11-10T08:00:11.1234567Z") > BpParseIsoTime(L"2026-11-01T09:00:00Z"));
    CHECK(BpParseIsoTime(L"") == 0);
    CHECK(BpParseIsoTime(NULL) == 0);
    CHECK(BpParseIsoTime(L"not a time") == 0);
    CHECK(BpParseIsoTime(L"2026-13-01T00:00:00Z") == 0);
    CHECK(BpParseIsoTime(L"2026-11-10 08:00:11Z") == 0);
}

static void TestParse(BP_EVENT *e)
{
    CHECK(BpParseEventXml(Boot100A, e) == 1);
    CHECK(e->EventId == 100);
    CHECK(e->Time == BpParseIsoTime(L"2026-11-10T08:00:11.1234567Z"));
    CHECK(BpGetUInt(e, L"BootTime", 0) == 71234);
    CHECK(BpGetUInt(e, L"bootTIME", 0) == 71234); // names are case-insensitive
    CHECK(BpGetUInt(e, L"Missing", 77) == 77);
    CHECK(BpGetUInt(e, L"BootStartTime", 5) == 2026); // leading digits parse; the caller only reads numeric fields
    CHECK(BpGetField(e, L"BootIsDegradation") && !wcscmp(BpGetField(e, L"BootIsDegradation"), L"true"));

    CHECK(BpParseEventXml(Boot100B, e) == 1 && e->EventId == 100); // Qualifiers attribute
    CHECK(BpParseEventXml(App101, e) == 1 && e->EventId == 101);
    CHECK(!wcscmp(BpGetField(e, L"Path"), L"C:\\Users\\a\\AppData\\Local\\Microsoft\\OneDrive\\OneDrive.exe"));

    CHECK(BpParseEventXml(Entities, e) == 1);
    CHECK(!wcscmp(BpGetField(e, L"Name"), L"A <b> \"q\" 's' AB &bogus; &amp;"));
    CHECK(BpGetField(e, L"Path") && BpGetField(e, L"Path")[0] == 0); // <Data Name='Path'/> is an empty value

    // not events
    CHECK(BpParseEventXml(L"", e) == 0);
    CHECK(BpParseEventXml(NULL, e) == 0);
    CHECK(BpParseEventXml(L"hello", e) == 0);
    CHECK(BpParseEventXml(L"<Event><System></System></Event>", e) == 0);
    CHECK(BpParseEventXml(L"<Event><System><EventID>abc</EventID></System></Event>", e) == 0);
    CHECK(BpParseEventXml(L"<Event><System><EventID>99999</EventID></System></Event>", e) == 0);
    // truncated at every length: never crashes, never reads past the end
    {
        size_t len = wcslen(App101);
        size_t n;
        wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));

        for (n = 0; n <= len; n++)
        {
            memcpy(copy, App101, n * sizeof(wchar_t));
            copy[n] = 0;
            BpParseEventXml(copy, e);
        }

        CHECK(1);
        free(copy);
    }
    // an over-long value is truncated, not overflowed
    {
        wchar_t *big = (wchar_t *)malloc(5000 * sizeof(wchar_t));
        size_t i;

        wcscpy(big, L"<Event><System><EventID>101</EventID></System><EventData><Data Name='Name'>");

        for (i = 0; i < 3000; i++)
            wcscat(big, L"x");

        wcscat(big, L"</Data></EventData></Event>");
        CHECK(BpParseEventXml(big, e) == 1);
        CHECK(wcslen(BpGetField(e, L"Name")) == BP_VALUE_CHARS - 1);
        free(big);
    }
    // more fields than fit
    {
        wchar_t *many = (wchar_t *)malloc(20000 * sizeof(wchar_t));
        int i;

        wcscpy(many, L"<Event><System><EventID>100</EventID></System><EventData>");

        for (i = 0; i < 200; i++)
        {
            wchar_t t[64];

            swprintf(t, 64, L"<Data Name='F%d'>%d</Data>", i, i);
            wcscat(many, t);
        }

        wcscat(many, L"</EventData></Event>");
        CHECK(BpParseEventXml(many, e) == 1 && e->FieldCount == BP_MAX_FIELDS);
        free(many);
    }
}

static void TestAggregate(BP_EVENT *e)
{
    BP_STATE s;
    const BP_ITEM *item;

    BpInit(&s);
    CHECK(!s.HasBoot && s.ItemCount == 0);
    CHECK(BpFindItem(&s, L"x", L"y") == NULL); // empty state

    CHECK(BpAddXml(&s, Boot100B, e) == 1); // older first
    CHECK(BpAddXml(&s, Boot100A, e) == 1);
    CHECK(s.HasBoot && s.BootCount == 2);
    CHECK(s.Boot.BootTimeMs == 71234 && s.Boot.MainPathBootTimeMs == 48000 && s.Boot.PostBootTimeMs == 23234);
    CHECK(s.Boot.DriverInitMs == 3500 && s.Boot.SmssInitMs == 8000 && s.Boot.ExplorerInitMs == 7000 && s.Boot.NumStartupApps == 14);
    CHECK(s.Boot.OsLoaderMs == 12000 && s.Boot.UserProfileMs == 5200);

    // order must not matter: newest boot wins either way
    {
        BP_STATE t;

        BpInit(&t);
        BpAddXml(&t, Boot100A, e);
        BpAddXml(&t, Boot100B, e);
        CHECK(t.Boot.BootTimeMs == 71234);
        BpFree(&t);
    }

    CHECK(BpAddXml(&s, App101Old, e) == 1);
    CHECK(BpAddXml(&s, App101, e) == 1);
    CHECK(BpAddXml(&s, Svc103, e) == 1);
    CHECK(BpAddXml(&s, Drv102, e) == 1);
    CHECK(BpAddXml(&s, Entities, e) == 1);
    CHECK(BpAddXml(&s, L"<Event><System><EventID>4001</EventID></System></Event>", e) == 1); // other id: parsed, ignored
    CHECK(BpAddXml(&s, L"garbage", e) == 0);
    CHECK(s.EventsIgnored == 2);
    CHECK(s.ItemCount == 4); // OneDrive (merged), SysMain, foo.sys, the entity one

    item = BpFindItem(&s, NULL, L"C:\\Users\\a\\AppData\\Local\\Microsoft\\OneDrive\\OneDrive.exe");
    CHECK(item && item->TotalMs == 6800 && item->DegradationMs == 4100 && item->Events == 2); // newest event wins
    CHECK(item && !wcscmp(item->Friendly, L"Microsoft OneDrive"));

    // matching: by base name (other directory, other case), by name without extension, by friendly name
    CHECK(BpFindItem(&s, NULL, L"D:\\Other\\onedrive.EXE") == item);
    CHECK(BpFindItem(&s, L"OneDrive", NULL) == item);
    CHECK(BpFindItem(&s, L"Microsoft OneDrive", L"C:\\nothing.exe") == item);
    CHECK(BpFindItem(&s, L"SysMain", NULL) && BpFindItem(&s, L"SysMain", NULL)->TotalMs == 1500);
    CHECK(BpFindItem(&s, NULL, L"C:\\Windows\\System32\\drivers\\foo.sys") && BpFindItem(&s, NULL, L"C:\\Windows\\System32\\drivers\\foo.sys")->TotalMs == 2500);
    CHECK(BpFindItem(&s, L"Dropbox", L"C:\\Dropbox\\Dropbox.exe") == NULL);
    CHECK(BpFindItem(&s, NULL, NULL) == NULL);
    CHECK(BpFindItem(&s, L"", L"") == NULL);
    CHECK(BpFindItem(&s, L"OneDriveX", L"C:\\x\\OneDriveX.exe") == NULL); // no prefix matching

    BpFree(&s);
    CHECK(s.Items == NULL && s.ItemCount == 0);
}

static void TestRating(void)
{
    CHECK(BpRateImpact(0) == 1);
    CHECK(BpRateImpact(999) == 1);
    CHECK(BpRateImpact(1000) == 2);
    CHECK(BpRateImpact(2999) == 2);
    CHECK(BpRateImpact(3000) == 3);
    CHECK(BpRateImpact(0xFFFFFFFFu) == 3);
}

int main(void)
{
    BP_EVENT *e = (BP_EVENT *)malloc(sizeof(BP_EVENT));

    TestTime();
    TestParse(e);
    TestAggregate(e);
    TestRating();
    free(e);
    printf("boot: %d checks, %d failed\n", Checks, Failures);
    return Failures ? 1 : 0;
}
