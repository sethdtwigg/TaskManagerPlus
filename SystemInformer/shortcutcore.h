/*
 * TaskManagerPlus: keyboard shortcut table, parsing, conflict detection and persistence.
 *
 * Pure C (no System Informer headers) so tools\tests\unit can compile it on its own.
 *
 * A shortcut is a modifier set (Ctrl, Shift, Alt) plus a virtual key, written "Ctrl+Shift+K", "F5" or "Ctrl+Tab".
 * Vk 0 means unbound. Letters, digits, F1..F24, Tab, Enter, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
 * the arrows and Pause are accepted. A key that types text (letters, digits, Space, Enter, Tab, Backspace, Delete, Insert,
 * Home, End, PageUp/Down, arrows) needs Ctrl or Alt so a binding never steals normal typing; F-keys and Pause may stand alone.
 * Ctrl+C/V/X/A/Z/Y (text editing), Alt+F4, Alt+Tab, Alt+Esc and Ctrl+Alt+Delete are reserved and refused.
 *
 * Persisted as one string of "id=shortcut" pairs separated by ';' holding only the entries that differ from their default
 * ("quickactions=Ctrl+Shift+K;refresh=none"). Reading is forgiving: unknown ids are ignored; a malformed, reserved or
 * conflicting shortcut falls back to that entry's default (or to unbound when the default is taken too).
 */

#ifndef _SHORTCUTCORE_H_
#define _SHORTCUTCORE_H_

#include <stddef.h>
#include <wchar.h>

#define SHC_MOD_CTRL 1u
#define SHC_MOD_SHIFT 2u
#define SHC_MOD_ALT 4u

typedef struct _SHC_COMBO
{
    unsigned Mods;
    unsigned Vk; // 0 = unbound
} SHC_COMBO;

typedef struct _SHC_ENTRY
{
    const char *Id;
    const wchar_t *Label;
    SHC_COMBO Default;
    SHC_COMBO Current;
} SHC_ENTRY;

enum
{
    ShcFind,
    ShcRun,
    ShcRunAs,
    ShcSave,
    ShcLog,
    ShcTabNext,
    ShcTabPrev,
    ShcRefresh,
    ShcSystemInfo,
    ShcPause,
    ShcQuickActions,
    ShcCount
};

typedef enum _SHC_CHECK
{
    ShcAllowed,
    ShcNeedsModifier,
    ShcReserved,
    ShcConflict
} SHC_CHECK;

#ifdef __cplusplus
extern "C" {
#endif

/** Fills Entries (ShcCount of them) with ids, labels and the default shortcuts; Current = Default. */
void ShcInitDefaults(SHC_ENTRY *Entries);

/** "Ctrl+Shift+K", "ctrl + tab", "F5" ; "" or "none" gives an unbound combo (Vk 0). Returns 1 on success. */
int ShcParse(const wchar_t *Text, SHC_COMBO *Combo);

/** Writes "Ctrl+Shift+K" style text ("None" when unbound). Returns the length, or 0 when it does not fit. */
size_t ShcFormat(SHC_COMBO Combo, wchar_t *Buffer, size_t Count);

int ShcEqual(SHC_COMBO A, SHC_COMBO B);

/** ShcAllowed, ShcNeedsModifier or ShcReserved for the combo on its own (unbound is allowed). */
SHC_CHECK ShcCheckCombo(SHC_COMBO Combo);

/** Index of another entry already using Combo, or -1. Unbound never conflicts. */
int ShcFindConflict(const SHC_ENTRY *Entries, size_t Count, size_t Index, SHC_COMBO Combo);

/** Full check of giving Entries[Index] the combo: reserved / modifier rules first, then conflicts (*Other is the owner). */
SHC_CHECK ShcCheckAssign(const SHC_ENTRY *Entries, size_t Count, size_t Index, SHC_COMBO Combo, int *Other);

/** Entry index bound to (Mods, Vk), or -1. */
int ShcLookup(const SHC_ENTRY *Entries, size_t Count, unsigned Mods, unsigned Vk);

/** Writes the persisted form; returns the length ((size_t)-1 when it does not fit). An all-default table gives "" (length 0). */
size_t ShcSerialize(const SHC_ENTRY *Entries, size_t Count, wchar_t *Buffer, size_t BufferCount);

/** Applies a persisted string on top of the defaults already in Entries. Never fails. */
void ShcDeserialize(SHC_ENTRY *Entries, size_t Count, const wchar_t *Text);

#ifdef __cplusplus
}
#endif

#endif
