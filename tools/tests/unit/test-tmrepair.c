/*
 * Unit tests for the Task Manager replacement repair decision (TaskManagerPlus): injected Debugger values and an
 * injected "file exists" callback, no registry access.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <string.h>
#include "../../../SystemInformer/tmrepair.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const wchar_t *Existing = NULL; // the only path the fake file system knows

static int FakeExists(const wchar_t *Path, void *Context)
{
    (void)Context;
    return Existing && TmrPathsEqual(Path, Existing);
}

static TMR_DECISION Decide(const wchar_t *Debugger, const wchar_t *Own, int Suppress, wchar_t *Stale)
{
    return TmrDecide(Debugger, Own, Suppress, FakeExists, NULL, Stale, 1024);
}

int main(void)
{
    wchar_t stale[1024];
    wchar_t path[64];
    const wchar_t *own = L"D:\\Tools\\TaskManagerPlus\\SystemInformer.exe";

    Existing = NULL;

    // extraction
    CHECK(TmrExtractPath(L"\"C:\\a b\\x.exe\"", stale, 1024) && !wcscmp(stale, L"C:\\a b\\x.exe"));
    CHECK(TmrExtractPath(L"C:\\a\\x.exe", stale, 1024) && !wcscmp(stale, L"C:\\a\\x.exe"));
    CHECK(TmrExtractPath(L"  \"C:\\a\\x.exe\" -arg", stale, 1024) && !wcscmp(stale, L"C:\\a\\x.exe"));
    CHECK(TmrExtractPath(L"\"C:\\unterminated.exe", stale, 1024) && !wcscmp(stale, L"C:\\unterminated.exe"));
    CHECK(!TmrExtractPath(L"", stale, 1024));
    CHECK(!TmrExtractPath(L"\"\"", stale, 1024));
    CHECK(!TmrExtractPath(L"   ", stale, 1024));
    CHECK(!TmrExtractPath(NULL, stale, 1024));
    CHECK(!TmrExtractPath(L"C:\\toolong.exe", path, 5)); // does not fit: refuse, never truncate

    // already correct (case and slash insensitive, quoted or not)
    CHECK(Decide(L"\"D:\\Tools\\TaskManagerPlus\\SystemInformer.exe\"", own, 0, stale) == TmrNothing);
    CHECK(Decide(L"d:/tools/taskmanagerplus/systeminformer.EXE", own, 0, stale) == TmrNothing);

    // nothing stored
    CHECK(Decide(NULL, own, 0, stale) == TmrNothing);
    CHECK(Decide(L"", own, 0, stale) == TmrNothing);

    // another program's debugger is never ours to touch, even if its file is missing
    CHECK(Decide(L"\"C:\\Program Files\\Other\\procexp64.exe\"", own, 0, stale) == TmrNotOurs);
    CHECK(Decide(L"vsjitdebugger.exe -p %ld", own, 0, stale) == TmrNotOurs);
    CHECK(stale[0] == 0);

    // dangling TaskManagerPlus path: offer, reporting the stale path unquoted
    CHECK(Decide(L"\"C:\\Old\\TaskManagerPlus.exe\"", own, 0, stale) == TmrOffer);
    CHECK(!wcscmp(stale, L"C:\\Old\\TaskManagerPlus.exe"));
    CHECK(Decide(L"E:\\Old\\SystemInformer.exe", own, 0, stale) == TmrOffer);
    CHECK(!wcscmp(stale, L"E:\\Old\\SystemInformer.exe"));
    CHECK(Decide(L"\"C:\\Old\\Renamed.exe\"", L"D:\\x\\Renamed.exe", 0, stale) == TmrOffer); // our own (renamed) file name

    // another working copy elsewhere: leave it alone
    Existing = L"C:\\Old\\TaskManagerPlus.exe";
    CHECK(Decide(L"\"C:\\Old\\TaskManagerPlus.exe\"", own, 0, stale) == TmrOtherCopyWorks);
    Existing = NULL;

    // "Don't ask again"
    CHECK(Decide(L"\"C:\\Old\\TaskManagerPlus.exe\"", own, 1, stale) == TmrSuppressed);
    CHECK(stale[0] == 0);
    // suppression does not make us touch a foreign value or change the "nothing" cases
    CHECK(Decide(L"\"C:\\Other\\x.exe\"", own, 1, stale) == TmrNotOurs);
    CHECK(Decide(NULL, own, 1, stale) == TmrNothing);

    // hostile / odd input does not crash and does not offer
    CHECK(Decide(L"\"", own, 0, stale) == TmrNothing);
    CHECK(Decide(L"\\", own, 0, stale) == TmrNotOurs);
    CHECK(Decide(L"\"C:\\a\\\\\"", own, 0, stale) == TmrNotOurs); // path ends in a backslash: empty base name
    CHECK(TmrDecide(L"C:\\Old\\TaskManagerPlus.exe", own, 0, FakeExists, NULL, NULL, 0) == TmrOffer); // no output buffer
    CHECK(TmrDecide(L"C:\\Old\\TaskManagerPlus.exe", NULL, 0, FakeExists, NULL, stale, 1024) == TmrNothing);

    printf("tmrepair: %d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
