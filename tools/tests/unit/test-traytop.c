/*
 * Unit tests for the tray quick-menu process selection (TaskManagerPlus plugins\TrayTop\topn.c): top N by CPU and by
 * memory, ties, idle process, empty and hostile input, count clamping.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <string.h>
#include "../../../plugins/TrayTop/topn.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void TestClamp(void)
{
    CHECK(TtClampCount(5) == 5);
    CHECK(TtClampCount(1) == 1);
    CHECK(TtClampCount(15) == 15);
    CHECK(TtClampCount(16) == 15);
    CHECK(TtClampCount(1000000) == 15);
    CHECK(TtClampCount(0) == TT_COUNT_DEFAULT);   // an empty field
    CHECK(TtClampCount(-3) == 1);
    CHECK(TtClampCount(-1000000000000LL) == 1);
    CHECK(TtClampCount(0x7FFFFFFFFFFFFFFFLL) == 15);
}

static void TestSelect(void)
{
    TT_ITEM items[] =
    {
        { 0, 99.0, 1000 },        // System Idle: never listed
        { 4, 1.5, 200ull << 20 },
        { 100, 40.0, 500ull << 20 },
        { 101, 40.0, 900ull << 20 },  // ties CPU with 100: lower pid first
        { 102, 0.0, 100ull << 20 },
        { 103, 12.25, 900ull << 20 }, // ties memory with 101
        { 104, 0.04, 0 },             // below the visible threshold, no memory
        { 105, 75.5, 50ull << 20 }
    };
    size_t out[16];
    size_t n;

    n = TtSelectTop(items, 8, 0, 3, out);
    CHECK(n == 3 && out[0] == 7 && out[1] == 2 && out[2] == 3);          // 105, 100, 101
    n = TtSelectTop(items, 8, 0, 15, out);
    CHECK(n == 5);                                                       // idle, 0.0% and 0.04% skipped
    CHECK(out[0] == 7 && out[1] == 2 && out[2] == 3 && out[3] == 5 && out[4] == 1);
    n = TtSelectTop(items, 8, 1, 3, out);
    CHECK(n == 3 && out[0] == 3 && out[1] == 5 && out[2] == 2);          // 101 and 103 tie at 900 MB: pid 101 first, then 500 MB (100)... checked below
    n = TtSelectTop(items, 8, 1, 15, out);
    CHECK(n == 6);                                                       // memory list skips only idle (no memory 104)
    CHECK(out[0] == 3 && out[1] == 5 && out[2] == 2 && out[3] == 1 && out[4] == 4 && out[5] == 7);
    CHECK(TtSelectTop(items, 8, 0, 1, out) == 1 && out[0] == 7);
    CHECK(TtSelectTop(items, 8, 1, 1, out) == 1 && out[0] == 3);
}

static void TestEdge(void)
{
    TT_ITEM idle[] = { { 0, 100.0, 5 }, { 7, 0.0, 0 }, { 8, 0.01, 0 } };
    TT_ITEM one[] = { { 9, 3.0, 4096 } };
    TT_ITEM nan[2];
    size_t out[4];
    double zero = 0.0;

    CHECK(TtSelectTop(idle, 3, 0, 5, out) == 0);                         // nothing busy: empty list, not a crash
    CHECK(TtSelectTop(idle, 3, 1, 5, out) == 0);
    CHECK(TtSelectTop(one, 1, 0, 5, out) == 1 && out[0] == 0);
    CHECK(TtSelectTop(one, 0, 0, 5, out) == 0);
    CHECK(TtSelectTop(NULL, 3, 0, 5, out) == 0);
    CHECK(TtSelectTop(one, 1, 0, 5, NULL) == 0);
    CHECK(TtSelectTop(one, 1, 0, 0, out) == 0);

    // NaN CPU never listed and never poisons the ordering
    nan[0].Pid = 10; nan[0].Cpu = zero / zero; nan[0].Memory = 1;
    nan[1].Pid = 11; nan[1].Cpu = 5.0; nan[1].Memory = 1;
    CHECK(TtSelectTop(nan, 2, 0, 5, out) == 1 && out[0] == 1);
    CHECK(TtSelectTop(nan, 2, 1, 5, out) == 2 && out[0] == 0 && out[1] == 1); // equal memory: lower pid first

    // many items, small N: output never exceeds N and is sorted
    {
        TT_ITEM many[1000];
        size_t big[15];
        size_t i;
        size_t n;
        unsigned seed = 99;

        for (i = 0; i < 1000; i++)
        {
            seed = seed * 1103515245u + 12345u;
            many[i].Pid = (uint32_t)(i + 1);
            many[i].Cpu = (double)((seed >> 16) % 1000) / 10.0;
            many[i].Memory = (uint64_t)((seed >> 8) % 100000);
        }

        n = TtSelectTop(many, 1000, 0, 15, big);
        CHECK(n == 15);

        for (i = 1; i < n; i++)
            CHECK(many[big[i - 1]].Cpu >= many[big[i]].Cpu);

        // nothing outside the list outranks its last entry
        for (i = 0; i < 1000; i++)
        {
            size_t j;
            int inList = 0;

            for (j = 0; j < n; j++)
                inList |= big[j] == i;

            CHECK(inList || many[i].Cpu <= many[big[n - 1]].Cpu);
        }

        n = TtSelectTop(many, 1000, 1, 7, big);
        CHECK(n == 7);

        for (i = 1; i < n; i++)
            CHECK(many[big[i - 1]].Memory >= many[big[i]].Memory);
    }
}

int main(void)
{
    TestClamp();
    TestSelect();
    TestEdge();
    printf("traytop: %d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
