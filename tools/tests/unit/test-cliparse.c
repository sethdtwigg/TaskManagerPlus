/*
 * Unit tests for the --report argument parsing (SessionReports cliparse.c): kind names and the format picked from the
 * file extension.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include "../../../plugins/SessionReports/cliparse.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    // kinds
    CHECK(SrCliParseKind(L"processes") == 0);
    CHECK(SrCliParseKind(L"Process") == 0);
    CHECK(SrCliParseKind(L"STARTUP") == 1);
    CHECK(SrCliParseKind(L"services") == 2);
    CHECK(SrCliParseKind(L"trends") == 3);
    CHECK(SrCliParseKind(L"") == -1);
    CHECK(SrCliParseKind(L"proc") == -1);
    CHECK(SrCliParseKind(L"processes ") == -1);
    CHECK(SrCliParseKind(L"..\\processes") == -1);
    CHECK(SrCliParseKind(NULL) == -1);

    // formats from the extension
    CHECK(SrCliFormatFromPath(L"out.csv") == 0);
    CHECK(SrCliFormatFromPath(L"C:\\x\\Out.CSV") == 0);
    CHECK(SrCliFormatFromPath(L"a.json") == 1);
    CHECK(SrCliFormatFromPath(L"a.JSON") == 1);
    CHECK(SrCliFormatFromPath(L"page.html") == 2);
    CHECK(SrCliFormatFromPath(L"page.HTM") == 2);
    CHECK(SrCliFormatFromPath(L"/tmp/a.b/report.html") == 2);
    CHECK(SrCliFormatFromPath(L"report.tar.json") == 1);
    CHECK(SrCliFormatFromPath(L"report") == -1);
    CHECK(SrCliFormatFromPath(L"report.txt") == -1);
    CHECK(SrCliFormatFromPath(L"report.csv.bak") == -1);
    CHECK(SrCliFormatFromPath(L"dir.csv\\noextension") == -1); // the dot is in a directory name
    CHECK(SrCliFormatFromPath(L"report.") == -1);
    CHECK(SrCliFormatFromPath(L".csv") == 0);
    CHECK(SrCliFormatFromPath(L"") == -1);
    CHECK(SrCliFormatFromPath(NULL) == -1);

    // exit codes are stable (documented)
    {
        volatile int codes[] = { SR_CLI_EXIT_OK, SR_CLI_EXIT_BAD_ARGS, SR_CLI_EXIT_NO_SOURCE, SR_CLI_EXIT_WRITE_FAILED, SR_CLI_EXIT_TIMEOUT };

        CHECK(codes[0] == 0 && codes[1] == 2 && codes[2] == 3 && codes[3] == 4 && codes[4] == 5);
    }

    printf("cliparse: %d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
