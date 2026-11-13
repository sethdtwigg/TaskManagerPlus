/*
 * Unit tests for Profiles (Phase 7 item 8): name validation, capture, apply plan (only differences, missing rules),
 * theme handling, apply then revert restores the exact previous state, store set/replace/remove, limits.
 * Built and run by tools\tests\unit-tests.ps1 (cl.exe only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../plugins/Profiles/profilecore.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond) \
    do { Checks++; if (!(cond)) { Failures++; printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

// "the machine": rules and a theme, mutated by applying a plan exactly like the plugin does
typedef struct _MACHINE
{
    PF_CURRENT_RULE Rules[8];
    uint32_t Count;
    int Theme;
} MACHINE;

static void Setup(MACHINE *m)
{
    memset(m, 0, sizeof(*m));
    wcscpy(m->Rules[0].Program, L"chrome.exe");  m->Rules[0].Enabled = 1; m->Rules[0].Priority = 5; // below normal
    wcscpy(m->Rules[1].Program, L"game.exe");    m->Rules[1].Enabled = 0; m->Rules[1].Priority = 0;
    wcscpy(m->Rules[2].Program, L"Backup.exe");  m->Rules[2].Enabled = 1; m->Rules[2].Priority = 1; // idle
    m->Count = 3;
    m->Theme = 2;
}

static void ApplyPlan(MACHINE *m, const PF_PLAN *p)
{
    uint32_t i, j;

    for (i = 0; i < p->ChangeCount; i++)
    {
        for (j = 0; j < m->Count; j++)
        {
            if (!_wcsicmp(m->Rules[j].Program, p->Changes[i].Program))
            {
                if (p->Changes[i].EnabledMode == PF_ON) m->Rules[j].Enabled = 1;
                if (p->Changes[i].EnabledMode == PF_OFF) m->Rules[j].Enabled = 0;
                if (p->Changes[i].HasPriority) m->Rules[j].Priority = p->Changes[i].Priority;
            }
        }
    }

    if (p->ThemeChange)
        m->Theme = (int)p->NewTheme;
}

static int Same(const MACHINE *a, const MACHINE *b)
{
    uint32_t i;

    if (a->Count != b->Count || a->Theme != b->Theme)
        return 0;

    for (i = 0; i < a->Count; i++)
    {
        if (wcscmp(a->Rules[i].Program, b->Rules[i].Program) || a->Rules[i].Enabled != b->Rules[i].Enabled || a->Rules[i].Priority != b->Rules[i].Priority)
            return 0;
    }

    return 1;
}

static void TestValidation(void)
{
    wchar_t longName[80];
    int i;

    CHECK(PfValidName(L"Work"));
    CHECK(PfValidName(L"Gaming (PC) é中"));
    CHECK(!PfValidName(L""));
    CHECK(!PfValidName(NULL));
    CHECK(!PfValidName(L"    "));
    CHECK(!PfValidName(L"bad\nname"));
    CHECK(!PfValidName(L"tab\there"));
    for (i = 0; i < 47; i++) longName[i] = L'a';
    longName[47] = 0;
    CHECK(PfValidName(longName));
    longName[47] = L'a';
    longName[48] = 0;
    CHECK(!PfValidName(longName));
    CHECK(PfThemeAllowed(0) && PfThemeAllowed(1) && PfThemeAllowed(2) && PfThemeAllowed(5));
    CHECK(!PfThemeAllowed(3) && !PfThemeAllowed(4) && !PfThemeAllowed(6) && !PfThemeAllowed(99));
}

static void TestNames(void)
{
    uint32_t v = 99;
    int m = 99;

    CHECK(PfPriorityFromName("BelowNormal", &v) && v == 5);
    CHECK(PfPriorityFromName("none", &v) && v == 0);
    CHECK(PfPriorityFromName("high", &v) && v == 3);
    CHECK(!PfPriorityFromName("realtime", &v) && !PfPriorityFromName("", &v) && !PfPriorityFromName(NULL, &v));
    CHECK(!strcmp(PfPriorityName(1), "idle") && !strcmp(PfPriorityName(6), "abovenormal") && PfPriorityName(4) == NULL);
    CHECK(PfThemeFromName("OLED", &v) && v == 5 && PfThemeFromName("dark", &v) && v == 2);
    CHECK(!PfThemeFromName("custom", &v) && !PfThemeFromName("purple", &v));
    CHECK(!strcmp(PfThemeName(0), "automatic") && PfThemeName(3) == NULL);
    CHECK(PfEnabledModeFromName("ON", &m) && m == PF_ON && PfEnabledModeFromName("off", &m) && m == PF_OFF && PfEnabledModeFromName("keep", &m) && m == PF_KEEP);
    CHECK(!PfEnabledModeFromName("maybe", &m));
    CHECK(!strcmp(PfEnabledModeName(PF_OFF), "off") && !strcmp(PfEnabledModeName(77), "keep"));
    // every allowed theme has a name and round-trips
    CHECK(PfThemeFromName(PfThemeName(5), &v) && v == 5);
}

static void TestCapture(void)
{
    MACHINE m;
    PF_PROFILE *p = PfAllocProfile();

    Setup(&m);
    CHECK(PfCapture(p, L"All", m.Rules, m.Count, m.Theme, PF_CAPTURE_ALL) == 0);
    CHECK(p->RuleCount == 3 && p->HasTheme && p->Theme == 2);
    CHECK(p->Rules[0].EnabledMode == PF_ON && p->Rules[0].HasPriority && p->Rules[0].Priority == 5);
    CHECK(p->Rules[1].EnabledMode == PF_OFF && p->Rules[1].HasPriority && p->Rules[1].Priority == 0); // "no priority change" is a value

    CHECK(PfCapture(p, L"EnabledOnly", m.Rules, m.Count, m.Theme, PF_CAPTURE_ENABLED) == 0);
    CHECK(p->RuleCount == 3 && !p->HasTheme && !p->Rules[0].HasPriority && p->Rules[1].EnabledMode == PF_OFF);

    CHECK(PfCapture(p, L"ThemeOnly", m.Rules, m.Count, m.Theme, PF_CAPTURE_THEME) == 0);
    CHECK(p->RuleCount == 0 && p->HasTheme && p->Theme == 2); // rules with nothing to remember are left out

    CHECK(PfCapture(p, L"CustomTheme", m.Rules, m.Count, 3, PF_CAPTURE_THEME) == 0 && !p->HasTheme); // custom is not captured
    CHECK(PfCapture(p, L"", m.Rules, m.Count, m.Theme, PF_CAPTURE_ALL) == -1);
    PfFreeProfile(p);
}

static void TestPlan(void)
{
    MACHINE m;
    PF_PROFILE *p = PfAllocProfile();
    PF_PLAN *plan = (PF_PLAN *)calloc(1, sizeof(PF_PLAN));

    Setup(&m);
    memset(p, 0, sizeof(*p));
    wcscpy(p->Name, L"Gaming");
    p->HasTheme = 1; p->Theme = 5;
    wcscpy(p->Rules[0].Program, L"CHROME.EXE"); p->Rules[0].EnabledMode = PF_OFF; // case-insensitive match
    wcscpy(p->Rules[1].Program, L"game.exe");   p->Rules[1].EnabledMode = PF_ON; p->Rules[1].HasPriority = 1; p->Rules[1].Priority = 3;
    wcscpy(p->Rules[2].Program, L"backup.exe"); p->Rules[2].EnabledMode = PF_ON; p->Rules[2].HasPriority = 1; p->Rules[2].Priority = 1; // already so
    wcscpy(p->Rules[3].Program, L"nothere.exe"); p->Rules[3].EnabledMode = PF_OFF;
    p->RuleCount = 4;

    PfPlanFor(p, m.Rules, m.Count, m.Theme, plan);
    CHECK(plan->ChangeCount == 2);
    CHECK(plan->ThemeChange && plan->NewTheme == 5);
    CHECK(plan->MissingRules == 1 && plan->Unchanged == 1);
    CHECK(plan->Changes[0].EnabledMode == PF_OFF && !plan->Changes[0].HasPriority);
    CHECK(plan->Changes[1].EnabledMode == PF_ON && plan->Changes[1].HasPriority && plan->Changes[1].Priority == 3);

    // applying it and planning again is empty
    ApplyPlan(&m, plan);
    PfPlanFor(p, m.Rules, m.Count, m.Theme, plan);
    CHECK(PfPlanIsEmpty(plan) && plan->MissingRules == 1 && plan->Unchanged == 3);

    // theme already matching: no theme change; disallowed theme in a hand-edited profile is ignored
    p->Theme = 3;
    m.Theme = 2;
    PfPlanFor(p, m.Rules, m.Count, m.Theme, plan);
    CHECK(!plan->ThemeChange);

    // empty profile, empty machine
    memset(p, 0, sizeof(*p));
    PfPlanFor(p, m.Rules, 0, 2, plan);
    CHECK(PfPlanIsEmpty(plan));

    free(plan);
    PfFreeProfile(p);
}

static void TestStoreApplyRevert(void)
{
    PF_STORE s;
    MACHINE m, original, afterWork;
    PF_PROFILE *work = PfAllocProfile();
    PF_PROFILE *game = PfAllocProfile();
    PF_PROFILE *src = NULL;
    PF_PLAN *plan = (PF_PLAN *)calloc(1, sizeof(PF_PLAN));

    PfStoreInit(&s);
    Setup(&m);
    original = m;

    // Work: chrome on, game off, theme light.  Gaming: chrome off, game on at high, theme OLED.
    memset(work, 0, sizeof(*work));
    wcscpy(work->Name, L"Work"); work->HasTheme = 1; work->Theme = 1;
    wcscpy(work->Rules[0].Program, L"chrome.exe"); work->Rules[0].EnabledMode = PF_ON;
    wcscpy(work->Rules[1].Program, L"game.exe"); work->Rules[1].EnabledMode = PF_OFF;
    work->RuleCount = 2;
    memset(game, 0, sizeof(*game));
    wcscpy(game->Name, L"Gaming"); game->HasTheme = 1; game->Theme = 5;
    wcscpy(game->Rules[0].Program, L"chrome.exe"); game->Rules[0].EnabledMode = PF_OFF;
    wcscpy(game->Rules[1].Program, L"game.exe"); game->Rules[1].EnabledMode = PF_ON; game->Rules[1].HasPriority = 1; game->Rules[1].Priority = 3;
    game->RuleCount = 2;

    CHECK(PfStoreSet(&s, work) == 0 && PfStoreSet(&s, game) == 0 && s.Count == 2);
    CHECK(PfStoreFind(&s, L"gaming") == 1 && PfStoreFind(&s, L"nope") == -1);

    CHECK(PfStoreApply(&s, L"Nope", m.Rules, m.Count, m.Theme, plan) == -1 && !s.Previous && !s.Active[0]);

    // apply Work: chrome already on, game off (change), theme light (change)
    CHECK(PfStoreApply(&s, L"work", m.Rules, m.Count, m.Theme, plan) == 0);
    CHECK(plan->ChangeCount == 0 && plan->ThemeChange && plan->NewTheme == 1);
    CHECK(!wcscmp(s.Active, L"Work") && s.Previous != NULL);
    ApplyPlan(&m, plan);
    CHECK(m.Theme == 1 && m.Rules[0].Enabled == 1 && m.Rules[1].Enabled == 0);
    afterWork = m;

    // apply Work again: nothing changes, and Previous must stay the ORIGINAL state (not the already-applied one)
    CHECK(PfStoreApply(&s, L"Work", m.Rules, m.Count, m.Theme, plan) == 0 && PfPlanIsEmpty(plan));
    {
        MACHINE prev;
        PF_PLAN *pp = (PF_PLAN *)calloc(1, sizeof(PF_PLAN));

        prev = m;
        // plan from the snapshot back onto the changed machine must reproduce the original
        PfPlanFor(s.Previous, prev.Rules, prev.Count, prev.Theme, pp);
        ApplyPlan(&prev, pp);
        CHECK(Same(&prev, &original));
        free(pp);
    }

    // switch to Gaming, then revert: back to the state after Work (Previous = state before the LAST effective apply)
    CHECK(PfStoreApply(&s, L"Gaming", m.Rules, m.Count, m.Theme, plan) == 0);
    CHECK(plan->ChangeCount == 2 && plan->ThemeChange && plan->NewTheme == 5);
    ApplyPlan(&m, plan);
    CHECK(m.Theme == 5 && m.Rules[0].Enabled == 0 && m.Rules[1].Enabled == 1 && m.Rules[1].Priority == 3);
    CHECK(PfStoreRevert(&s, m.Rules, m.Count, m.Theme, plan, &src) == 0 && src != NULL);
    ApplyPlan(&m, plan);
    CHECK(Same(&m, &afterWork));
    PfFreeProfile(src);
    CHECK(s.Previous == NULL && !s.Active[0]);
    CHECK(PfStoreRevert(&s, m.Rules, m.Count, m.Theme, plan, &src) == -1 && src == NULL); // nothing left to revert

    // a rule deleted between apply and revert is simply missing
    CHECK(PfStoreApply(&s, L"Gaming", m.Rules, m.Count, m.Theme, plan) == 0);
    ApplyPlan(&m, plan);
    m.Count = 2; // the Backup.exe rule is gone
    CHECK(PfStoreRevert(&s, m.Rules, m.Count, m.Theme, plan, &src) == 0);
    CHECK(plan->MissingRules == 1);
    ApplyPlan(&m, plan);
    CHECK(m.Theme == 1 && m.Rules[0].Enabled == 1);
    PfFreeProfile(src);

    // replace by name (case-insensitive), remove, active cleared on removal
    memcpy(work, game, sizeof(*work));
    wcscpy(work->Name, L"WORK");
    CHECK(PfStoreSet(&s, work) == 0 && s.Count == 2 && s.Profiles[0]->Theme == 5);
    PfStoreApply(&s, L"Gaming", m.Rules, m.Count, m.Theme, plan);
    CHECK(!wcscmp(s.Active, L"Gaming"));
    CHECK(PfStoreRemove(&s, L"gaming") == 0 && s.Count == 1 && !s.Active[0]);
    CHECK(PfStoreRemove(&s, L"gaming") == -1);

    PfStoreFree(&s);
    PfFreeProfile(work);
    PfFreeProfile(game);
    free(plan);
}

static void TestLimits(void)
{
    PF_STORE s;
    PF_PROFILE *p = PfAllocProfile();
    int i;

    PfStoreInit(&s);
    memset(p, 0, sizeof(*p));

    for (i = 0; i < PF_MAX_PROFILES; i++)
    {
        swprintf(p->Name, PF_NAME_CHARS, L"P%d", i);
        CHECK(PfStoreSet(&s, p) == 0);
    }

    swprintf(p->Name, PF_NAME_CHARS, L"One too many");
    CHECK(PfStoreSet(&s, p) == -2);
    swprintf(p->Name, PF_NAME_CHARS, L"p3");
    CHECK(PfStoreSet(&s, p) == 0 && s.Count == PF_MAX_PROFILES); // replacing is allowed when full

    wcscpy(p->Name, L"bad theme");
    p->HasTheme = 1; p->Theme = 3;
    CHECK(PfStoreSet(&s, p) == -1);
    p->HasTheme = 0;
    p->RuleCount = PF_MAX_RULES + 1;
    CHECK(PfStoreSet(&s, p) == -1);
    p->RuleCount = 0;
    wcscpy(p->Name, L"");
    CHECK(PfStoreSet(&s, p) == -1);

    PfStoreFree(&s);
    PfFreeProfile(p);
}

int main(void)
{
    TestValidation();
    TestNames();
    TestCapture();
    TestPlan();
    TestStoreApplyRevert();
    TestLimits();
    printf("profiles: %d checks, %d failed\n", Checks, Failures);
    return Failures ? 1 : 0;
}
