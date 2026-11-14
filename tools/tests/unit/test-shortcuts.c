/*
 * Unit tests for the keyboard shortcut table (TaskManagerPlus SystemInformer\shortcutcore.c): parsing and formatting,
 * reserved and unsafe combinations, conflict detection, lookup, persistence round trip and forgiving loading of damaged
 * or hostile settings strings.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <string.h>
#include "../../../SystemInformer/shortcutcore.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static SHC_COMBO Combo(unsigned Mods, unsigned Vk)
{
    SHC_COMBO c;

    c.Mods = Mods;
    c.Vk = Vk;
    return c;
}

static int ParseOk(const wchar_t *Text, unsigned Mods, unsigned Vk)
{
    SHC_COMBO c;

    return ShcParse(Text, &c) && c.Mods == Mods && c.Vk == Vk;
}

static void TestParseFormat(void)
{
    wchar_t text[64];
    SHC_COMBO c;
    unsigned vk;

    CHECK(ParseOk(L"Ctrl+K", SHC_MOD_CTRL, 'K'));
    CHECK(ParseOk(L"ctrl+k", SHC_MOD_CTRL, 'K'));
    CHECK(ParseOk(L"  Ctrl + Shift + R ", SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R'));
    CHECK(ParseOk(L"Shift+Ctrl+R", SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R'));
    CHECK(ParseOk(L"Control+Alt+1", SHC_MOD_CTRL | SHC_MOD_ALT, '1'));
    CHECK(ParseOk(L"F5", 0, 0x74));
    CHECK(ParseOk(L"f12", 0, 0x7B));
    CHECK(ParseOk(L"F24", 0, 0x87));
    CHECK(ParseOk(L"Ctrl+Tab", SHC_MOD_CTRL, 0x09));
    CHECK(ParseOk(L"Ctrl+Shift+Tab", SHC_MOD_CTRL | SHC_MOD_SHIFT, 0x09));
    CHECK(ParseOk(L"Ctrl+PageDown", SHC_MOD_CTRL, 0x22));
    CHECK(ParseOk(L"alt+left", SHC_MOD_ALT, 0x25));
    CHECK(ParseOk(L"Pause", 0, 0x13));
    CHECK(ParseOk(L"", 0, 0));
    CHECK(ParseOk(L"none", 0, 0));
    CHECK(ParseOk(L"None  ", 0, 0));
    CHECK(ParseOk(L"   ", 0, 0));

    CHECK(!ShcParse(L"Ctrl+", &c));
    CHECK(!ShcParse(L"+K", &c));
    CHECK(!ShcParse(L"Ctrl+Ctrl+K", &c));
    CHECK(!ShcParse(L"Ctrl+K+L", &c));
    CHECK(!ShcParse(L"K+Ctrl", &c));
    CHECK(!ShcParse(L"Ctrl+Shift", &c));
    CHECK(!ShcParse(L"Ctrl", &c));
    CHECK(!ShcParse(L"Ctrl+Foo", &c));
    CHECK(!ShcParse(L"Ctrl+F0", &c));
    CHECK(!ShcParse(L"Ctrl+F25", &c));
    CHECK(!ShcParse(L"Ctrl+F01", &c));
    CHECK(!ShcParse(L"Ctrl+F1x", &c));
    CHECK(!ShcParse(L"Ctrl+KK", &c));
    CHECK(!ShcParse(L"Win+K", &c));
    CHECK(!ShcParse(L"Ctrl++", &c));
    CHECK(!ShcParse(L"Ctrl+;", &c));
    CHECK(!ShcParse(NULL, &c));
    CHECK(!ShcParse(L"F5", NULL));

    // format round trip for every key we accept, with every modifier set
    for (vk = 1; vk < 256; vk++)
    {
        unsigned mods;

        for (mods = 0; mods < 8; mods++)
        {
            SHC_COMBO in = Combo(mods, vk);
            SHC_COMBO out;
            size_t length = ShcFormat(in, text, 64);

            if (length == 0)
                continue; // not a key we name

            CHECK(length == wcslen(text));
            CHECK(ShcParse(text, &out) && out.Mods == mods && out.Vk == vk);
        }
    }

    CHECK(ShcFormat(Combo(0, 0), text, 64) == 4 && !wcscmp(text, L"None"));
    CHECK(ShcFormat(Combo(SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R'), text, 64) > 0 && !wcscmp(text, L"Ctrl+Shift+R"));
    CHECK(ShcFormat(Combo(0, 0x75), text, 64) > 0 && !wcscmp(text, L"F6"));
    CHECK(ShcFormat(Combo(SHC_MOD_CTRL, 0x09), text, 64) > 0 && !wcscmp(text, L"Ctrl+Tab"));
    CHECK(ShcFormat(Combo(SHC_MOD_CTRL | SHC_MOD_ALT | SHC_MOD_SHIFT, 0x81), text, 64) > 0 && !wcscmp(text, L"Ctrl+Shift+Alt+F18"));

    // a small buffer never overruns and reports failure
    {
        wchar_t tiny[6];
        size_t n;

        for (n = 1; n < 6; n++)
        {
            wchar_t guard[16];

            guard[n] = 0x5A5A;
            CHECK(ShcFormat(Combo(SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R'), guard, n) == 0);
            CHECK(guard[n] == 0x5A5A);
        }

        CHECK(ShcFormat(Combo(0, 'A'), tiny, 0) == 0);
        CHECK(ShcFormat(Combo(0, 'A'), NULL, 5) == 0);
    }
}

static void TestCheck(void)
{
    CHECK(ShcCheckCombo(Combo(0, 0)) == ShcAllowed);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'K')) == ShcAllowed);
    CHECK(ShcCheckCombo(Combo(0, 0x74)) == ShcAllowed);              // F5 alone
    CHECK(ShcCheckCombo(Combo(0, 0x13)) == ShcAllowed);              // Pause alone
    CHECK(ShcCheckCombo(Combo(SHC_MOD_SHIFT, 0x74)) == ShcAllowed);  // Shift+F5
    CHECK(ShcCheckCombo(Combo(0, 'K')) == ShcNeedsModifier);         // would steal typing
    CHECK(ShcCheckCombo(Combo(SHC_MOD_SHIFT, 'K')) == ShcNeedsModifier);
    CHECK(ShcCheckCombo(Combo(0, 0x20)) == ShcNeedsModifier);
    CHECK(ShcCheckCombo(Combo(0, 0x0D)) == ShcNeedsModifier);
    CHECK(ShcCheckCombo(Combo(0, 0x25)) == ShcNeedsModifier);
    CHECK(ShcCheckCombo(Combo(0, 0x2E)) == ShcNeedsModifier);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'C')) == ShcReserved);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'V')) == ShcReserved);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'X')) == ShcReserved);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'A')) == ShcReserved);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'Z')) == ShcReserved);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL, 'Y')) == ShcReserved);
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL | SHC_MOD_SHIFT, 'C')) == ShcAllowed); // not the clipboard key
    CHECK(ShcCheckCombo(Combo(SHC_MOD_ALT, 0x73)) == ShcReserved);                // Alt+F4
    CHECK(ShcCheckCombo(Combo(SHC_MOD_ALT, 0x09)) == ShcReserved);                // Alt+Tab
    CHECK(ShcCheckCombo(Combo(SHC_MOD_ALT, 0x1B)) == ShcReserved);                // Alt+Esc
    CHECK(ShcCheckCombo(Combo(SHC_MOD_CTRL | SHC_MOD_ALT, 0x2E)) == ShcReserved); // Ctrl+Alt+Del
    CHECK(ShcCheckCombo(Combo(SHC_MOD_ALT, 'M')) == ShcAllowed);
}

static void TestConflicts(void)
{
    SHC_ENTRY e[ShcCount];
    int other = -1;
    int i;

    ShcInitDefaults(e);

    // defaults are valid and conflict-free
    for (i = 0; i < ShcCount; i++)
    {
        CHECK(e[i].Id && e[i].Label && e[i].Default.Vk);
        CHECK(ShcCheckAssign(e, ShcCount, (size_t)i, e[i].Current, &other) == ShcAllowed);
        CHECK(ShcLookup(e, ShcCount, e[i].Current.Mods, e[i].Current.Vk) == i);
    }

    // taking another entry's shortcut is a conflict that names the owner
    CHECK(ShcCheckAssign(e, ShcCount, ShcQuickActions, Combo(SHC_MOD_CTRL, 'R'), &other) == ShcConflict && other == ShcRun);
    CHECK(ShcFindConflict(e, ShcCount, ShcRun, Combo(SHC_MOD_CTRL, 'R')) == -1);       // not a conflict with itself
    CHECK(ShcFindConflict(e, ShcCount, ShcQuickActions, Combo(SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R')) == ShcRunAs);
    CHECK(ShcFindConflict(e, ShcCount, ShcQuickActions, Combo(SHC_MOD_CTRL, 'Q')) == -1);
    CHECK(ShcFindConflict(e, ShcCount, ShcQuickActions, Combo(0, 0)) == -1);           // unbound never conflicts
    CHECK(ShcCheckAssign(e, ShcCount, ShcQuickActions, Combo(SHC_MOD_CTRL, 'C'), &other) == ShcReserved && other == -1);
    CHECK(ShcCheckAssign(e, ShcCount, ShcQuickActions, Combo(0, 'Q'), &other) == ShcNeedsModifier);

    // two entries unbound do not collide
    e[ShcFind].Current = Combo(0, 0);
    e[ShcLog].Current = Combo(0, 0);
    CHECK(ShcFindConflict(e, ShcCount, ShcRun, Combo(0, 0)) == -1);
    CHECK(ShcLookup(e, ShcCount, 0, 0) == -1);

    // lookup distinguishes modifier sets
    ShcInitDefaults(e);
    CHECK(ShcLookup(e, ShcCount, SHC_MOD_CTRL, 'R') == ShcRun);
    CHECK(ShcLookup(e, ShcCount, SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R') == ShcRunAs);
    CHECK(ShcLookup(e, ShcCount, 0, 'R') == -1);
    CHECK(ShcLookup(e, ShcCount, SHC_MOD_CTRL, 'K') == ShcQuickActions);
}

static void TestPersistence(void)
{
    SHC_ENTRY e[ShcCount];
    SHC_ENTRY f[ShcCount];
    wchar_t text[512];
    size_t n;
    int i;

    ShcInitDefaults(e);
    CHECK(ShcSerialize(e, ShcCount, text, 512) == 0 && text[0] == 0);   // all defaults: empty

    e[ShcQuickActions].Current = Combo(SHC_MOD_CTRL | SHC_MOD_SHIFT, 'K');
    e[ShcRefresh].Current = Combo(0, 0);
    e[ShcLog].Current = Combo(SHC_MOD_ALT, 'L');
    n = ShcSerialize(e, ShcCount, text, 512);
    CHECK(n == wcslen(text) && n > 0);
    CHECK(wcsstr(text, L"quickactions=Ctrl+Shift+K") != NULL);
    CHECK(wcsstr(text, L"refresh=None") != NULL);
    CHECK(wcsstr(text, L"run=") == NULL);                                // defaults are not written

    ShcInitDefaults(f);
    ShcDeserialize(f, ShcCount, text);

    for (i = 0; i < ShcCount; i++)
        CHECK(ShcEqual(f[i].Current, e[i].Current));

    // too small a buffer reports failure instead of overrunning
    for (n = 1; n < 40; n++)
    {
        wchar_t small[64];

        small[n] = 0x5A5A;
        CHECK(ShcSerialize(e, ShcCount, small, n) == (size_t)-1);
        CHECK(small[n] == 0x5A5A);
    }

    // forgiving loading
    ShcInitDefaults(f);
    ShcDeserialize(f, ShcCount, L"nosuch=Ctrl+Q;;;=Ctrl+W;run;quickactions=banana;refresh=Ctrl+;log=Ctrl+Alt+L;junk");
    CHECK(ShcEqual(f[ShcQuickActions].Current, f[ShcQuickActions].Default)); // bad text: default kept
    CHECK(ShcEqual(f[ShcRefresh].Current, f[ShcRefresh].Default));
    CHECK(ShcEqual(f[ShcRun].Current, f[ShcRun].Default));
    CHECK(ShcEqual(f[ShcLog].Current, Combo(SHC_MOD_CTRL | SHC_MOD_ALT, 'L')));

    // a reserved or modifier-less binding falls back to the default
    ShcInitDefaults(f);
    ShcDeserialize(f, ShcCount, L"find=Ctrl+C;save=K;pause=Alt+F4;log=Ctrl+Alt+Delete");
    CHECK(ShcEqual(f[ShcFind].Current, f[ShcFind].Default));
    CHECK(ShcEqual(f[ShcSave].Current, f[ShcSave].Default));
    CHECK(ShcEqual(f[ShcPause].Current, f[ShcPause].Default));
    CHECK(ShcEqual(f[ShcLog].Current, f[ShcLog].Default));

    // two entries given the same combo: the earlier one keeps it, the later falls back to its default
    ShcInitDefaults(f);
    ShcDeserialize(f, ShcCount, L"refresh=Ctrl+M;pause=Ctrl+M");
    CHECK(ShcEqual(f[ShcRefresh].Current, Combo(SHC_MOD_CTRL, 'M')));
    CHECK(ShcEqual(f[ShcPause].Current, f[ShcPause].Default));

    // an entry given another entry's (kept) default: the later one in table order loses
    ShcInitDefaults(f);
    ShcDeserialize(f, ShcCount, L"quickactions=Ctrl+F");
    CHECK(ShcEqual(f[ShcFind].Current, Combo(SHC_MOD_CTRL, 'F')));
    CHECK(ShcEqual(f[ShcQuickActions].Current, f[ShcQuickActions].Default));

    // swap two shortcuts in the file: both ends keep consistent, no duplicates remain
    ShcInitDefaults(f);
    ShcDeserialize(f, ShcCount, L"find=Ctrl+K;quickactions=Ctrl+F");
    {
        int a;
        int b;

        for (a = 0; a < ShcCount; a++)
            for (b = a + 1; b < ShcCount; b++)
                CHECK(!f[a].Current.Vk || !ShcEqual(f[a].Current, f[b].Current));
    }

    // hostile strings never crash or leave duplicates
    {
        unsigned seed = 7;
        int round;

        for (round = 0; round < 5000; round++)
        {
            wchar_t junk[120];
            int k;
            int a;
            int b;
            static const wchar_t soup[] = L"=;+ CtrlShiftAltF1234567890KRPausenoneTab quickactionsrunfindrefresh";

            for (k = 0; k < 119; k++)
            {
                seed = seed * 1103515245u + 12345u;
                junk[k] = (round & 1) ? (wchar_t)(seed >> 16) : soup[(seed >> 16) % (sizeof(soup) / sizeof(soup[0]) - 1)];
            }

            junk[119] = 0;
            ShcInitDefaults(f);
            ShcDeserialize(f, ShcCount, junk);

            for (a = 0; a < ShcCount; a++)
            {
                CHECK(ShcCheckCombo(f[a].Current) == ShcAllowed);

                for (b = a + 1; b < ShcCount; b++)
                    CHECK(!f[a].Current.Vk || !ShcEqual(f[a].Current, f[b].Current));
            }
        }
    }

    ShcDeserialize(NULL, ShcCount, L"x=y");
    ShcDeserialize(f, ShcCount, NULL);
}

int main(void)
{
    TestParseFormat();
    TestCheck();
    TestConflicts();
    TestPersistence();
    printf("shortcuts: %d checks, %d failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
