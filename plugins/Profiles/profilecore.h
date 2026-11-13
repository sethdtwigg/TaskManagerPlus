/*
 * TaskManagerPlus Profiles: the profile model, the apply plan and the revert snapshot.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 *
 * A profile has a name, an optional theme mode and a list of per-rule settings. A rule is identified by its program name
 * (case-insensitive), so profiles survive exporting to another machine. Per rule a profile may set the enabled state
 * (keep / on / off) and the rule's start priority (keep, or a PROCESS_PRIORITY_CLASS_* value where 0 means "the rule does
 * not change priority").
 *
 * Applying a profile:
 *   1. PfStoreApply compares the profile with the current rules and theme and returns a plan that lists only what differs
 *      (so applying twice changes nothing) plus how many named rules do not exist here.
 *   2. When the plan changes something, a snapshot of the CURRENT state of every rule and the theme is kept as the
 *      "previous" settings. PfStoreRevert plans a return to that snapshot and clears it.
 * Themes allowed: 0 automatic, 1 light, 2 dark, 5 OLED (3 = custom needs the colour editor, 4 = Explorer is not offered).
 */

#ifndef _PF_PROFILECORE_H_
#define _PF_PROFILECORE_H_

#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#define PF_MAX_PROFILES 32
#define PF_MAX_RULES 128
#define PF_NAME_CHARS 48
#define PF_PROGRAM_CHARS 260

#define PF_KEEP 0
#define PF_ON 1
#define PF_OFF 2

#define PF_CAPTURE_ENABLED 0x1
#define PF_CAPTURE_PRIORITY 0x2
#define PF_CAPTURE_THEME 0x4
#define PF_CAPTURE_ALL 0x7

typedef struct _PF_RULE_SETTING
{
    wchar_t Program[PF_PROGRAM_CHARS];
    int EnabledMode; // PF_KEEP / PF_ON / PF_OFF
    int HasPriority; // 1 = set the rule's start priority to Priority
    uint32_t Priority;
} PF_RULE_SETTING;

/** About 70 KB: always allocate with PfAllocProfile. */
typedef struct _PF_PROFILE
{
    wchar_t Name[PF_NAME_CHARS];
    int HasTheme;
    uint32_t Theme;
    uint32_t RuleCount;
    PF_RULE_SETTING Rules[PF_MAX_RULES];
} PF_PROFILE;

/** The current state of one process rule. */
typedef struct _PF_CURRENT_RULE
{
    wchar_t Program[PF_PROGRAM_CHARS];
    int Enabled;
    uint32_t Priority;
} PF_CURRENT_RULE;

typedef struct _PF_CHANGE
{
    const wchar_t *Program; // points into the profile that produced the plan
    int EnabledMode; // PF_KEEP when the enabled state already matches
    int HasPriority;
    uint32_t Priority;
} PF_CHANGE;

typedef struct _PF_PLAN
{
    uint32_t ChangeCount;
    PF_CHANGE Changes[PF_MAX_RULES];
    int ThemeChange; // 1 = set the theme to NewTheme
    uint32_t NewTheme;
    uint32_t MissingRules; // named in the profile, no such rule here
    uint32_t Unchanged; // already as the profile wants
} PF_PLAN;

typedef struct _PF_STORE
{
    PF_PROFILE *Profiles[PF_MAX_PROFILES];
    uint32_t Count;
    wchar_t Active[PF_NAME_CHARS]; // empty = none
    PF_PROFILE *Previous; // snapshot taken before the last apply, NULL when there is nothing to revert
} PF_STORE;

#ifdef __cplusplus
extern "C" {
#endif

PF_PROFILE *PfAllocProfile(void);
void PfFreeProfile(PF_PROFILE *Profile);

/** 1 when the name is usable: 1..47 characters, no control characters, not only spaces. */
int PfValidName(const wchar_t *Name);

/** 1 for the theme modes a profile may set (0, 1, 2, 5). */
int PfThemeAllowed(uint32_t Theme);

/** Builds a profile from the current state. Flags select what is captured (PF_CAPTURE_*). Returns 0, or -1 for a bad name. */
int PfCapture(PF_PROFILE *Out, const wchar_t *Name, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, unsigned Flags);

/**
 * Names used in the JSON files (human editable). Priority classes follow phlib: 1 idle, 2 normal, 3 high, 5 below normal,
 * 6 above normal; 0 is "none" (the rule does not change priority). Real time (4) is not offered. The From functions return 1
 * and set *Value when the name is known (case-insensitive).
 */
const char *PfPriorityName(uint32_t Priority);
int PfPriorityFromName(const char *Name, uint32_t *Value);
const char *PfThemeName(uint32_t Theme);
int PfThemeFromName(const char *Name, uint32_t *Value);
const char *PfEnabledModeName(int Mode);
int PfEnabledModeFromName(const char *Name, int *Mode);

/** What has to change to bring the current state to the profile. */
void PfPlanFor(const PF_PROFILE *Profile, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, PF_PLAN *Plan);

int PfPlanIsEmpty(const PF_PLAN *Plan);

void PfStoreInit(PF_STORE *Store);
void PfStoreFree(PF_STORE *Store);

/** Index of the profile with that name (case-insensitive) or -1. */
int PfStoreFind(const PF_STORE *Store, const wchar_t *Name);

/** Adds a copy, or replaces the profile of the same name. 0 ok, -1 invalid (name, theme, rule count), -2 store full. */
int PfStoreSet(PF_STORE *Store, const PF_PROFILE *Profile);

/** 0 removed, -1 not found. Clears Active when it was the removed one. */
int PfStoreRemove(PF_STORE *Store, const wchar_t *Name);

/**
 * Applies the named profile: fills Plan (the caller carries it out), sets Active, and when the plan changes anything
 * stores the current state as Previous. Returns 0, or -1 when there is no such profile.
 */
int PfStoreApply(PF_STORE *Store, const wchar_t *Name, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, PF_PLAN *Plan);

/**
 * Plans the return to Previous and clears it (and Active). The detached snapshot is returned in Source (the plan points into it:
 * keep it until the plan has been carried out, then PfFreeProfile). Returns 0, or -1 when there is nothing to revert.
 */
int PfStoreRevert(PF_STORE *Store, const PF_CURRENT_RULE *Rules, uint32_t Count, int CurrentTheme, PF_PLAN *Plan, PF_PROFILE **Source);

#ifdef __cplusplus
}
#endif

#endif
