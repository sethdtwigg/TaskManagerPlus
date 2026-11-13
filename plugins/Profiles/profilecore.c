/*
 * TaskManagerPlus Profiles: profile model, apply plan and revert snapshot. See profilecore.h.
 */

#include "profilecore.h"
#include <stdlib.h>
#include <string.h>

PF_PROFILE *PfAllocProfile(void)
{
    return (PF_PROFILE *)calloc(1, sizeof(PF_PROFILE));
}

void PfFreeProfile(PF_PROFILE *Profile)
{
    free(Profile);
}

static void PfCopyString(wchar_t *Dest, size_t Capacity, const wchar_t *Src)
{
    size_t n = 0;

    if (Src)
    {
        while (Src[n] && n + 1 < Capacity)
        {
            Dest[n] = Src[n];
            n++;
        }
    }

    Dest[n] = 0;
}

int PfValidName(const wchar_t *Name)
{
    size_t i;
    size_t length;
    int visible = 0;

    if (!Name)
        return 0;

    length = wcslen(Name);

    if (length == 0 || length >= PF_NAME_CHARS)
        return 0;

    for (i = 0; i < length; i++)
    {
        if (Name[i] < 0x20 || Name[i] == 0x7F)
            return 0;

        if (Name[i] != L' ')
            visible = 1;
    }

    return visible;
}

int PfThemeAllowed(uint32_t Theme)
{
    return Theme == 0 || Theme == 1 || Theme == 2 || Theme == 5;
}

static const struct { const char *Name; uint32_t Value; } PfPriorities[] =
{
    { "none", 0 }, { "idle", 1 }, { "belownormal", 5 }, { "normal", 2 }, { "abovenormal", 6 }, { "high", 3 }
};

static const struct { const char *Name; uint32_t Value; } PfThemes[] =
{
    { "automatic", 0 }, { "light", 1 }, { "dark", 2 }, { "oled", 5 }
};

static const struct { const char *Name; int Value; } PfModes[] =
{
    { "keep", PF_KEEP }, { "on", PF_ON }, { "off", PF_OFF }
};

const char *PfPriorityName(uint32_t Priority)
{
    size_t i;

    for (i = 0; i < sizeof(PfPriorities) / sizeof(PfPriorities[0]); i++)
    {
        if (PfPriorities[i].Value == Priority)
            return PfPriorities[i].Name;
    }

    return NULL;
}

int PfPriorityFromName(const char *Name, uint32_t *Value)
{
    size_t i;

    for (i = 0; Name && i < sizeof(PfPriorities) / sizeof(PfPriorities[0]); i++)
    {
        if (!_stricmp(Name, PfPriorities[i].Name))
        {
            *Value = PfPriorities[i].Value;
            return 1;
        }
    }

    return 0;
}

const char *PfThemeName(uint32_t Theme)
{
    size_t i;

    for (i = 0; i < sizeof(PfThemes) / sizeof(PfThemes[0]); i++)
    {
        if (PfThemes[i].Value == Theme)
            return PfThemes[i].Name;
    }

    return NULL;
}

int PfThemeFromName(const char *Name, uint32_t *Value)
{
    size_t i;

    for (i = 0; Name && i < sizeof(PfThemes) / sizeof(PfThemes[0]); i++)
    {
        if (!_stricmp(Name, PfThemes[i].Name))
        {
            *Value = PfThemes[i].Value;
            return 1;
        }
    }

    return 0;
}

const char *PfEnabledModeName(int Mode)
{
    size_t i;

    for (i = 0; i < sizeof(PfModes) / sizeof(PfModes[0]); i++)
    {
        if (PfModes[i].Value == Mode)
            return PfModes[i].Name;
    }

    return "keep";
}

int PfEnabledModeFromName(const char *Name, int *Mode)
{
    size_t i;

    for (i = 0; Name && i < sizeof(PfModes) / sizeof(PfModes[0]); i++)
    {
        if (!_stricmp(Name, PfModes[i].Name))
        {
            *Mode = PfModes[i].Value;
            return 1;
        }
    }

    return 0;
}

int PfCapture(PF_PROFILE *Out, const wchar_t *Name, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, unsigned Flags)
{
    uint32_t i;

    if (!PfValidName(Name))
        return -1;

    memset(Out, 0, sizeof(*Out));
    PfCopyString(Out->Name, PF_NAME_CHARS, Name);

    if ((Flags & PF_CAPTURE_THEME) && PfThemeAllowed((uint32_t)CurrentTheme))
    {
        Out->HasTheme = 1;
        Out->Theme = (uint32_t)CurrentTheme;
    }

    for (i = 0; i < Count && Out->RuleCount < PF_MAX_RULES; i++)
    {
        PF_RULE_SETTING *s = &Out->Rules[Out->RuleCount];

        if (!Rules[i].Program[0])
            continue;

        PfCopyString(s->Program, PF_PROGRAM_CHARS, Rules[i].Program);
        s->EnabledMode = (Flags & PF_CAPTURE_ENABLED) ? (Rules[i].Enabled ? PF_ON : PF_OFF) : PF_KEEP;
        s->HasPriority = (Flags & PF_CAPTURE_PRIORITY) ? 1 : 0;
        s->Priority = (Flags & PF_CAPTURE_PRIORITY) ? Rules[i].Priority : 0;

        if (s->EnabledMode == PF_KEEP && !s->HasPriority)
            continue; // nothing to remember for this rule

        Out->RuleCount++;
    }

    return 0;
}

static const PF_CURRENT_RULE *PfFindRule(const PF_CURRENT_RULE *Rules, uint32_t Count, const wchar_t *Program)
{
    uint32_t i;

    for (i = 0; i < Count; i++)
    {
        if (!_wcsicmp(Rules[i].Program, Program))
            return &Rules[i];
    }

    return NULL;
}

void PfPlanFor(const PF_PROFILE *Profile, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, PF_PLAN *Plan)
{
    uint32_t i;

    memset(Plan, 0, sizeof(*Plan));

    if (Profile->HasTheme && PfThemeAllowed(Profile->Theme) && (int)Profile->Theme != CurrentTheme)
    {
        Plan->ThemeChange = 1;
        Plan->NewTheme = Profile->Theme;
    }

    for (i = 0; i < Profile->RuleCount && i < PF_MAX_RULES; i++)
    {
        const PF_RULE_SETTING *s = &Profile->Rules[i];
        const PF_CURRENT_RULE *cur = PfFindRule(Rules, Count, s->Program);
        PF_CHANGE *c;
        int enabledDiffers;
        int priorityDiffers;

        if (!cur)
        {
            Plan->MissingRules++;
            continue;
        }

        enabledDiffers = (s->EnabledMode == PF_ON && !cur->Enabled) || (s->EnabledMode == PF_OFF && cur->Enabled);
        priorityDiffers = s->HasPriority && s->Priority != cur->Priority;

        if (!enabledDiffers && !priorityDiffers)
        {
            Plan->Unchanged++;
            continue;
        }

        c = &Plan->Changes[Plan->ChangeCount++];
        c->Program = s->Program;
        c->EnabledMode = enabledDiffers ? s->EnabledMode : PF_KEEP;
        c->HasPriority = priorityDiffers;
        c->Priority = priorityDiffers ? s->Priority : 0;
    }
}

int PfPlanIsEmpty(const PF_PLAN *Plan)
{
    return Plan->ChangeCount == 0 && !Plan->ThemeChange;
}

void PfStoreInit(PF_STORE *Store)
{
    memset(Store, 0, sizeof(*Store));
}

void PfStoreFree(PF_STORE *Store)
{
    uint32_t i;

    for (i = 0; i < Store->Count; i++)
        free(Store->Profiles[i]);

    free(Store->Previous);
    memset(Store, 0, sizeof(*Store));
}

int PfStoreFind(const PF_STORE *Store, const wchar_t *Name)
{
    uint32_t i;

    if (!Name)
        return -1;

    for (i = 0; i < Store->Count; i++)
    {
        if (!_wcsicmp(Store->Profiles[i]->Name, Name))
            return (int)i;
    }

    return -1;
}

int PfStoreSet(PF_STORE *Store, const PF_PROFILE *Profile)
{
    int index;
    PF_PROFILE *copy;

    if (!PfValidName(Profile->Name) || Profile->RuleCount > PF_MAX_RULES || (Profile->HasTheme && !PfThemeAllowed(Profile->Theme)))
        return -1;

    index = PfStoreFind(Store, Profile->Name);

    if (index < 0 && Store->Count >= PF_MAX_PROFILES)
        return -2;

    copy = PfAllocProfile();

    if (!copy)
        return -2;

    memcpy(copy, Profile, sizeof(*copy));
    copy->Name[PF_NAME_CHARS - 1] = 0;

    if (index >= 0)
    {
        free(Store->Profiles[index]);
        Store->Profiles[index] = copy;
    }
    else
    {
        Store->Profiles[Store->Count++] = copy;
    }

    return 0;
}

int PfStoreRemove(PF_STORE *Store, const wchar_t *Name)
{
    int index = PfStoreFind(Store, Name);
    uint32_t i;

    if (index < 0)
        return -1;

    if (!_wcsicmp(Store->Active, Store->Profiles[index]->Name))
        Store->Active[0] = 0;

    free(Store->Profiles[index]);

    for (i = (uint32_t)index; i + 1 < Store->Count; i++)
        Store->Profiles[i] = Store->Profiles[i + 1];

    Store->Profiles[--Store->Count] = NULL;
    return 0;
}

int PfStoreApply(PF_STORE *Store, const wchar_t *Name, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, PF_PLAN *Plan)
{
    int index = PfStoreFind(Store, Name);

    if (index < 0)
    {
        memset(Plan, 0, sizeof(*Plan));
        return -1;
    }

    PfPlanFor(Store->Profiles[index], Rules, Count, CurrentTheme, Plan);

    if (!PfPlanIsEmpty(Plan))
    {
        PF_PROFILE *snapshot = PfAllocProfile();

        if (snapshot && PfCapture(snapshot, L"previous", Rules, Count, CurrentTheme, PF_CAPTURE_ALL) == 0)
        {
            free(Store->Previous);
            Store->Previous = snapshot;
        }
        else
        {
            free(snapshot);
        }
    }

    PfCopyString(Store->Active, PF_NAME_CHARS, Store->Profiles[index]->Name);
    return 0;
}

int PfStoreRevert(PF_STORE *Store, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, PF_PLAN *Plan, PF_PROFILE **Source)
{
    *Source = NULL;

    if (!Store->Previous)
    {
        memset(Plan, 0, sizeof(*Plan));
        return -1;
    }

    *Source = Store->Previous;
    Store->Previous = NULL;
    Store->Active[0] = 0;
    PfPlanFor(*Source, Rules, Count, CurrentTheme, Plan);
    return 0;
}
