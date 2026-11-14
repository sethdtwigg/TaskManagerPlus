/*
 * TaskManagerPlus: keyboard shortcut table, parsing, conflict detection and persistence (see shortcutcore.h).
 */

#include "shortcutcore.h"

#define VK_BACK_ 0x08
#define VK_TAB_ 0x09
#define VK_RETURN_ 0x0D
#define VK_PAUSE_ 0x13
#define VK_SPACE_ 0x20
#define VK_PRIOR_ 0x21
#define VK_NEXT_ 0x22
#define VK_END_ 0x23
#define VK_HOME_ 0x24
#define VK_LEFT_ 0x25
#define VK_UP_ 0x26
#define VK_RIGHT_ 0x27
#define VK_DOWN_ 0x28
#define VK_INSERT_ 0x2D
#define VK_DELETE_ 0x2E
#define VK_F1_ 0x70
#define VK_ESCAPE_ 0x1B

static const struct
{
    const wchar_t *Name;
    unsigned Vk;
} ShcNamedKeys[] =
{
    { L"Tab", VK_TAB_ }, { L"Enter", VK_RETURN_ }, { L"Space", VK_SPACE_ }, { L"Backspace", VK_BACK_ },
    { L"Delete", VK_DELETE_ }, { L"Insert", VK_INSERT_ }, { L"Home", VK_HOME_ }, { L"End", VK_END_ },
    { L"PageUp", VK_PRIOR_ }, { L"PageDown", VK_NEXT_ }, { L"Left", VK_LEFT_ }, { L"Right", VK_RIGHT_ },
    { L"Up", VK_UP_ }, { L"Down", VK_DOWN_ }, { L"Pause", VK_PAUSE_ }
};

#define SHC_NAMED_KEY_COUNT (sizeof(ShcNamedKeys) / sizeof(ShcNamedKeys[0]))

void ShcInitDefaults(SHC_ENTRY *Entries)
{
    static const struct
    {
        const char *Id;
        const wchar_t *Label;
        unsigned Mods;
        unsigned Vk;
    } defaults[ShcCount] =
    {
        { "find", L"Find handles or DLLs", SHC_MOD_CTRL, 'F' },
        { "run", L"Run...", SHC_MOD_CTRL, 'R' },
        { "runas", L"Run as...", SHC_MOD_CTRL | SHC_MOD_SHIFT, 'R' },
        { "save", L"Save...", SHC_MOD_CTRL, 'S' },
        { "log", L"Log", SHC_MOD_CTRL, 'L' },
        { "tabnext", L"Next tab", SHC_MOD_CTRL, VK_TAB_ },
        { "tabprev", L"Previous tab", SHC_MOD_CTRL | SHC_MOD_SHIFT, VK_TAB_ },
        { "refresh", L"Refresh", 0, VK_F1_ + 4 },
        { "sysinfo", L"System information", SHC_MOD_CTRL, 'I' },
        { "pause", L"Pause / resume updates", 0, VK_F1_ + 5 },
        { "quickactions", L"Quick actions palette", SHC_MOD_CTRL, 'K' }
    };
    int i;

    for (i = 0; i < ShcCount; i++)
    {
        Entries[i].Id = defaults[i].Id;
        Entries[i].Label = defaults[i].Label;
        Entries[i].Default.Mods = defaults[i].Mods;
        Entries[i].Default.Vk = defaults[i].Vk;
        Entries[i].Current = Entries[i].Default;
    }
}

static wchar_t ShcUpper(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c;
}

static int ShcEqualNoCase(const wchar_t *A, size_t ALength, const wchar_t *B)
{
    size_t i;

    for (i = 0; i < ALength; i++)
    {
        if (!B[i] || ShcUpper(A[i]) != ShcUpper(B[i]))
            return 0;
    }

    return !B[ALength];
}

/** Parses one key name (no modifiers) of ALength characters into a virtual key, or 0. */
static unsigned ShcParseKey(const wchar_t *Text, size_t Length)
{
    size_t i;

    if (Length == 1)
    {
        wchar_t c = ShcUpper(Text[0]);

        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            return (unsigned)c;
    }

    if ((Text[0] == L'F' || Text[0] == L'f') && Length >= 2 && Length <= 3)
    {
        unsigned n = 0;

        for (i = 1; i < Length; i++)
        {
            if (Text[i] < L'0' || Text[i] > L'9')
                return 0;

            n = n * 10 + (unsigned)(Text[i] - L'0');
        }

        if (Text[1] != L'0' && n >= 1 && n <= 24)
            return VK_F1_ + n - 1;

        return 0;
    }

    for (i = 0; i < SHC_NAMED_KEY_COUNT; i++)
    {
        if (ShcEqualNoCase(Text, Length, ShcNamedKeys[i].Name))
            return ShcNamedKeys[i].Vk;
    }

    return 0;
}

int ShcParse(const wchar_t *Text, SHC_COMBO *Combo)
{
    SHC_COMBO result = { 0, 0 };
    const wchar_t *p;
    int sawKey = 0;

    if (!Text || !Combo)
        return 0;

    while (*Text == L' ' || *Text == L'\t')
        Text++;

    {
        size_t trimmed = wcslen(Text);

        while (trimmed && (Text[trimmed - 1] == L' ' || Text[trimmed - 1] == L'	'))
            trimmed--;

        if (!trimmed || ShcEqualNoCase(Text, trimmed, L"none"))
        {
            *Combo = result;
            return 1;
        }
    }

    p = Text;

    for (;;)
    {
        const wchar_t *start;
        const wchar_t *end;
        size_t length;

        while (*p == L' ' || *p == L'\t')
            p++;

        start = p;

        while (*p && *p != L'+')
            p++;

        end = p;

        while (end > start && (end[-1] == L' ' || end[-1] == L'\t'))
            end--;

        length = (size_t)(end - start);

        if (!length)
            return 0;

        if (sawKey)
            return 0; // something after the key

        if (ShcEqualNoCase(start, length, L"Ctrl") || ShcEqualNoCase(start, length, L"Control"))
        {
            if (result.Mods & SHC_MOD_CTRL)
                return 0;

            result.Mods |= SHC_MOD_CTRL;
        }
        else if (ShcEqualNoCase(start, length, L"Shift"))
        {
            if (result.Mods & SHC_MOD_SHIFT)
                return 0;

            result.Mods |= SHC_MOD_SHIFT;
        }
        else if (ShcEqualNoCase(start, length, L"Alt"))
        {
            if (result.Mods & SHC_MOD_ALT)
                return 0;

            result.Mods |= SHC_MOD_ALT;
        }
        else
        {
            result.Vk = ShcParseKey(start, length);

            if (!result.Vk)
                return 0;

            sawKey = 1;
        }

        if (!*p)
            break;

        p++; // the '+'
    }

    if (!sawKey)
        return 0;

    *Combo = result;
    return 1;
}

static size_t ShcAppend(wchar_t *Buffer, size_t Count, size_t Used, const wchar_t *Text)
{
    while (*Text)
    {
        if (Used + 1 >= Count)
            return (size_t)-1;

        Buffer[Used++] = *Text++;
    }

    Buffer[Used] = 0;
    return Used;
}

/** Writes the name of a virtual key into Out (at least 12 characters). Returns 1 when the key is one we know. */
static int ShcKeyName(unsigned Vk, wchar_t *Out)
{
    size_t i;

    if ((Vk >= 'A' && Vk <= 'Z') || (Vk >= '0' && Vk <= '9'))
    {
        Out[0] = (wchar_t)Vk;
        Out[1] = 0;
        return 1;
    }

    if (Vk >= VK_F1_ && Vk < VK_F1_ + 24)
    {
        unsigned n = Vk - VK_F1_ + 1;
        size_t l = 0;

        Out[l++] = L'F';

        if (n >= 10)
            Out[l++] = (wchar_t)(L'0' + n / 10);

        Out[l++] = (wchar_t)(L'0' + n % 10);
        Out[l] = 0;
        return 1;
    }

    for (i = 0; i < SHC_NAMED_KEY_COUNT; i++)
    {
        if (ShcNamedKeys[i].Vk == Vk)
        {
            size_t l = 0;

            while (ShcNamedKeys[i].Name[l])
            {
                Out[l] = ShcNamedKeys[i].Name[l];
                l++;
            }

            Out[l] = 0;
            return 1;
        }
    }

    return 0;
}

size_t ShcFormat(SHC_COMBO Combo, wchar_t *Buffer, size_t Count)
{
    size_t used = 0;
    wchar_t key[12];

    if (!Buffer || !Count)
        return 0;

    Buffer[0] = 0;

    if (!Combo.Vk)
        return ShcAppend(Buffer, Count, 0, L"None") == (size_t)-1 ? 0 : 4;

    if (!ShcKeyName(Combo.Vk, key))
        return 0;

    if (Combo.Mods & SHC_MOD_CTRL)
        used = ShcAppend(Buffer, Count, used, L"Ctrl+");

    if (used != (size_t)-1 && (Combo.Mods & SHC_MOD_SHIFT))
        used = ShcAppend(Buffer, Count, used, L"Shift+");

    if (used != (size_t)-1 && (Combo.Mods & SHC_MOD_ALT))
        used = ShcAppend(Buffer, Count, used, L"Alt+");

    if (used != (size_t)-1)
        used = ShcAppend(Buffer, Count, used, key);

    if (used == (size_t)-1)
    {
        Buffer[0] = 0;
        return 0;
    }

    return used;
}

int ShcEqual(SHC_COMBO A, SHC_COMBO B)
{
    return A.Vk == B.Vk && (A.Vk == 0 || A.Mods == B.Mods);
}

SHC_CHECK ShcCheckCombo(SHC_COMBO Combo)
{
    unsigned m = Combo.Mods;
    unsigned k = Combo.Vk;

    if (!k)
        return ShcAllowed;

    if ((m == SHC_MOD_CTRL) && (k == 'C' || k == 'V' || k == 'X' || k == 'A' || k == 'Z' || k == 'Y'))
        return ShcReserved;

    if (m == SHC_MOD_ALT && (k == VK_F1_ + 3 || k == VK_TAB_ || k == VK_ESCAPE_))
        return ShcReserved;

    if (m == (SHC_MOD_CTRL | SHC_MOD_ALT) && k == VK_DELETE_)
        return ShcReserved;

    if (!(m & (SHC_MOD_CTRL | SHC_MOD_ALT)))
    {
        // F-keys and Pause may stand alone; everything else would steal typing
        if ((k >= VK_F1_ && k < VK_F1_ + 24) || k == VK_PAUSE_)
            return ShcAllowed;

        return ShcNeedsModifier;
    }

    return ShcAllowed;
}

int ShcFindConflict(const SHC_ENTRY *Entries, size_t Count, size_t Index, SHC_COMBO Combo)
{
    size_t i;

    if (!Combo.Vk)
        return -1;

    for (i = 0; i < Count; i++)
    {
        if (i != Index && Entries[i].Current.Vk && ShcEqual(Entries[i].Current, Combo))
            return (int)i;
    }

    return -1;
}

SHC_CHECK ShcCheckAssign(const SHC_ENTRY *Entries, size_t Count, size_t Index, SHC_COMBO Combo, int *Other)
{
    SHC_CHECK check = ShcCheckCombo(Combo);
    int other;

    if (Other)
        *Other = -1;

    if (check != ShcAllowed)
        return check;

    other = ShcFindConflict(Entries, Count, Index, Combo);

    if (other >= 0)
    {
        if (Other)
            *Other = other;

        return ShcConflict;
    }

    return ShcAllowed;
}

int ShcLookup(const SHC_ENTRY *Entries, size_t Count, unsigned Mods, unsigned Vk)
{
    size_t i;

    if (!Vk)
        return -1;

    for (i = 0; i < Count; i++)
    {
        if (Entries[i].Current.Vk == Vk && Entries[i].Current.Mods == Mods)
            return (int)i;
    }

    return -1;
}

static size_t ShcAppendAscii(wchar_t *Buffer, size_t Count, size_t Used, const char *Text)
{
    while (*Text)
    {
        if (Used + 1 >= Count)
            return (size_t)-1;

        Buffer[Used++] = (wchar_t)(unsigned char)*Text++;
    }

    Buffer[Used] = 0;
    return Used;
}

size_t ShcSerialize(const SHC_ENTRY *Entries, size_t Count, wchar_t *Buffer, size_t BufferCount)
{
    size_t used = 0;
    size_t i;

    if (!Buffer || !BufferCount)
        return (size_t)-1;

    Buffer[0] = 0;

    for (i = 0; i < Count; i++)
    {
        wchar_t text[48];

        if (ShcEqual(Entries[i].Current, Entries[i].Default))
            continue;

        if (used && (used = ShcAppend(Buffer, BufferCount, used, L";")) == (size_t)-1)
            return (size_t)-1;

        if ((used = ShcAppendAscii(Buffer, BufferCount, used, Entries[i].Id)) == (size_t)-1 ||
            (used = ShcAppend(Buffer, BufferCount, used, L"=")) == (size_t)-1)
            return (size_t)-1;

        if (!ShcFormat(Entries[i].Current, text, 48))
            return (size_t)-1;

        if ((used = ShcAppend(Buffer, BufferCount, used, text)) == (size_t)-1)
            return (size_t)-1;
    }

    return used;
}

void ShcDeserialize(SHC_ENTRY *Entries, size_t Count, const wchar_t *Text)
{
    const wchar_t *p = Text;
    size_t i;

    if (!Entries || !Text)
        return;

    while (*p)
    {
        const wchar_t *idStart = p;
        const wchar_t *equals = NULL;
        const wchar_t *valueStart;
        const wchar_t *end;
        wchar_t value[48];
        size_t idLength;
        size_t valueLength;

        while (*p && *p != L';')
        {
            if (*p == L'=' && !equals)
                equals = p;

            p++;
        }

        end = p;

        if (*p == L';')
            p++;

        if (!equals)
            continue;

        idLength = (size_t)(equals - idStart);
        valueStart = equals + 1;
        valueLength = (size_t)(end - valueStart);

        if (!idLength || valueLength >= 48)
            continue;

        for (i = 0; i < valueLength; i++)
            value[i] = valueStart[i];

        value[valueLength] = 0;

        for (i = 0; i < Count; i++)
        {
            size_t n = 0;

            while (Entries[i].Id[n] && n < idLength && (wchar_t)(unsigned char)Entries[i].Id[n] == idStart[n])
                n++;

            if (!Entries[i].Id[n] && n == idLength)
            {
                SHC_COMBO combo;

                if (ShcParse(value, &combo))
                    Entries[i].Current = combo;

                break;
            }
        }
    }

    // Make the result consistent: a combo that is reserved, lacks a modifier or is already taken by an earlier
    // entry goes back to that entry's default, or to unbound when the default is taken as well.
    for (i = 0; i < Count; i++)
    {
        SHC_CHECK check;
        SHC_COMBO current = Entries[i].Current;
        size_t j;
        int taken = 0;

        check = ShcCheckCombo(current);

        for (j = 0; j < i && !taken; j++)
            taken = current.Vk && ShcEqual(Entries[j].Current, current);

        if (check == ShcAllowed && !taken)
            continue;

        Entries[i].Current = Entries[i].Default;
        taken = 0;

        for (j = 0; j < i && !taken; j++)
            taken = Entries[i].Current.Vk && ShcEqual(Entries[j].Current, Entries[i].Current);

        if (taken)
        {
            Entries[i].Current.Mods = 0;
            Entries[i].Current.Vk = 0;
        }
    }

    // An earlier entry that was reverted can now collide with a later one that kept its custom combo: rerun until stable.
    for (i = 0; i < Count; i++)
    {
        size_t j;

        for (j = i + 1; j < Count; j++)
        {
            if (Entries[i].Current.Vk && ShcEqual(Entries[i].Current, Entries[j].Current))
            {
                Entries[j].Current = Entries[j].Default;

                if (Entries[j].Current.Vk && ShcEqual(Entries[i].Current, Entries[j].Current))
                {
                    Entries[j].Current.Mods = 0;
                    Entries[j].Current.Vk = 0;
                }
            }
        }
    }
}
