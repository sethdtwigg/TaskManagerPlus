/*
 * TaskManagerPlus StartupManager: boot event parsing. See bootparse.h.
 */

#include "bootparse.h"
#include <stdlib.h>
#include <string.h>

static int BpIsSpace(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

/** Decodes XML text between [Start, End) into Out (at most Capacity-1 characters, always terminated). */
static void BpDecode(const wchar_t *Start, const wchar_t *End, wchar_t *Out, size_t Capacity)
{
    size_t n = 0;

    while (Start < End && n + 1 < Capacity)
    {
        if (*Start == L'&')
        {
            const wchar_t *semi = Start + 1;
            wchar_t ch = 0;

            while (semi < End && *semi != L';' && semi - Start < 10)
                semi++;

            if (semi < End && *semi == L';')
            {
                size_t len = (size_t)(semi - Start - 1);
                const wchar_t *e = Start + 1;

                if (len == 3 && !wcsncmp(e, L"amp", 3)) ch = L'&';
                else if (len == 2 && !wcsncmp(e, L"lt", 2)) ch = L'<';
                else if (len == 2 && !wcsncmp(e, L"gt", 2)) ch = L'>';
                else if (len == 4 && !wcsncmp(e, L"quot", 4)) ch = L'"';
                else if (len == 4 && !wcsncmp(e, L"apos", 4)) ch = L'\'';
                else if (len >= 2 && e[0] == L'#')
                {
                    unsigned long v = 0;
                    size_t i = 1;
                    int hex = (e[1] == L'x' || e[1] == L'X');

                    if (hex)
                        i = 2;

                    for (; i < len; i++)
                    {
                        wchar_t d = e[i];
                        int dv = (d >= L'0' && d <= L'9') ? d - L'0' : hex && d >= L'a' && d <= L'f' ? d - L'a' + 10 : hex && d >= L'A' && d <= L'F' ? d - L'A' + 10 : -1;

                        if (dv < 0)
                        {
                            v = 0;
                            break;
                        }

                        v = v * (hex ? 16 : 10) + (unsigned long)dv;

                        if (v > 0xFFFF)
                        {
                            v = 0xFFFD;
                            break;
                        }
                    }

                    ch = v ? (wchar_t)v : L'?';
                }

                if (ch)
                {
                    Out[n++] = ch;
                    Start = semi + 1;
                    continue;
                }
            }
        }

        Out[n++] = *Start++;
    }

    Out[n] = 0;
}

/** Finds the value of attribute Name inside the tag text [Tag, TagEnd). Returns 1 and the value range. */
static int BpAttribute(const wchar_t *Tag, const wchar_t *TagEnd, const wchar_t *Name, const wchar_t **VStart, const wchar_t **VEnd)
{
    size_t nameLen = wcslen(Name);
    const wchar_t *p = Tag;

    while (p + nameLen + 2 < TagEnd)
    {
        if (BpIsSpace(p[0]) && !wcsncmp(p + 1, Name, nameLen))
        {
            const wchar_t *q = p + 1 + nameLen;

            while (q < TagEnd && BpIsSpace(*q))
                q++;

            if (q < TagEnd && *q == L'=')
            {
                wchar_t quote;

                q++;

                while (q < TagEnd && BpIsSpace(*q))
                    q++;

                if (q < TagEnd && (*q == L'\'' || *q == L'"'))
                {
                    quote = *q++;
                    *VStart = q;

                    while (q < TagEnd && *q != quote)
                        q++;

                    if (q < TagEnd)
                    {
                        *VEnd = q;
                        return 1;
                    }
                }
            }
        }

        p++;
    }

    return 0;
}

int64_t BpParseIsoTime(const wchar_t *Text)
{
    int y, mo, d, h, mi, s;
    int64_t frac = 0;
    int digits = 0;
    int64_t days;
    int64_t yy;
    int64_t era;
    int64_t yoe;
    int64_t doy;
    int64_t doe;
    const wchar_t *p = Text;
    int i;
    int vals[6];
    static const int widths[6] = { 4, 2, 2, 2, 2, 2 };
    static const wchar_t seps[6] = { L'-', L'-', L'T', L':', L':', 0 };

    if (!Text)
        return 0;

    for (i = 0; i < 6; i++)
    {
        int v = 0;
        int w;

        for (w = 0; w < widths[i]; w++)
        {
            if (p[w] < L'0' || p[w] > L'9')
                return 0;

            v = v * 10 + (p[w] - L'0');
        }

        p += widths[i];
        vals[i] = v;

        if (seps[i])
        {
            if (*p != seps[i])
                return 0;

            p++;
        }
    }

    y = vals[0]; mo = vals[1]; d = vals[2]; h = vals[3]; mi = vals[4]; s = vals[5];

    if (*p == L'.')
    {
        p++;

        while (*p >= L'0' && *p <= L'9')
        {
            if (digits < 7)
            {
                frac = frac * 10 + (*p - L'0');
                digits++;
            }

            p++;
        }

        while (digits < 7)
        {
            frac *= 10;
            digits++;
        }
    }

    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60 || y < 1601)
        return 0;

    // days since 1601-01-01 (civil from days, H. Hinnant)
    yy = mo <= 2 ? y - 1 : y;
    era = (yy >= 0 ? yy : yy - 399) / 400;
    yoe = yy - era * 400;
    doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468; // since 1970-01-01
    days += 134774; // 1601-01-01 to 1970-01-01

    return ((days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s) * 10000000LL) + frac;
}

int BpParseEventXml(const wchar_t *Xml, BP_EVENT *Out)
{
    const wchar_t *p;
    int found = 0;

    memset(Out, 0, sizeof(*Out));

    if (!Xml)
        return 0;

    p = Xml;

    while (*p)
    {
        const wchar_t *lt = wcschr(p, L'<');
        const wchar_t *gt;

        if (!lt)
            break;

        gt = wcschr(lt, L'>');

        if (!gt)
            break;

        if (!wcsncmp(lt, L"<EventID", 8) && (BpIsSpace(lt[8]) || lt[8] == L'>'))
        {
            if (gt[-1] != L'/')
            {
                const wchar_t *close = wcsstr(gt + 1, L"</EventID>");

                if (close && !found)
                {
                    wchar_t text[16];
                    wchar_t *end;
                    unsigned long id;

                    BpDecode(gt + 1, close, text, 16);
                    id = wcstoul(text, &end, 10);

                    if (end != text && id <= 0xFFFF)
                    {
                        Out->EventId = (uint32_t)id;
                        found = 1;
                    }
                }
            }
        }
        else if (!wcsncmp(lt, L"<TimeCreated", 12) && BpIsSpace(lt[12]))
        {
            const wchar_t *vs, *ve;

            if (BpAttribute(lt, gt, L"SystemTime", &vs, &ve))
            {
                wchar_t text[48];

                BpDecode(vs, ve, text, 48);
                Out->Time = BpParseIsoTime(text);
            }
        }
        else if (!wcsncmp(lt, L"<Data", 5) && (BpIsSpace(lt[5]) || lt[5] == L'>' || lt[5] == L'/'))
        {
            const wchar_t *vs, *ve;

            if (BpAttribute(lt, gt, L"Name", &vs, &ve) && Out->FieldCount < BP_MAX_FIELDS)
            {
                BP_FIELD *f = &Out->Fields[Out->FieldCount];

                BpDecode(vs, ve, f->Name, BP_NAME_CHARS);

                if (gt[-1] == L'/')
                {
                    f->Value[0] = 0;
                }
                else
                {
                    const wchar_t *close = wcsstr(gt + 1, L"</Data>");

                    if (close)
                        BpDecode(gt + 1, close, f->Value, BP_VALUE_CHARS);
                    else
                        f->Value[0] = 0;
                }

                Out->FieldCount++;
            }
        }

        p = gt + 1;
    }

    return found;
}

const wchar_t *BpGetField(const BP_EVENT *Event, const wchar_t *Name)
{
    uint32_t i;

    for (i = 0; i < Event->FieldCount; i++)
    {
        if (!_wcsicmp(Event->Fields[i].Name, Name))
            return Event->Fields[i].Value;
    }

    return NULL;
}

uint32_t BpGetUInt(const BP_EVENT *Event, const wchar_t *Name, uint32_t Default)
{
    const wchar_t *v = BpGetField(Event, Name);
    wchar_t *end;
    unsigned long long n;

    if (!v || !*v)
        return Default;

    n = wcstoull(v, &end, 10);

    if (end == v)
        return Default;

    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

void BpInit(BP_STATE *State)
{
    memset(State, 0, sizeof(*State));
}

void BpFree(BP_STATE *State)
{
    free(State->Items);
    memset(State, 0, sizeof(*State));
}

static void BpCopy(wchar_t *Dest, const wchar_t *Src)
{
    size_t n = 0;

    if (!Src)
        Src = L"";

    while (Src[n] && n < BP_ITEM_CHARS - 1)
    {
        Dest[n] = Src[n];
        n++;
    }

    Dest[n] = 0;
}

static const wchar_t *BpBaseName(const wchar_t *Path)
{
    const wchar_t *b = Path;
    const wchar_t *p;

    for (p = Path; *p; p++)
    {
        if (*p == L'\\' || *p == L'/')
            b = p + 1;
    }

    return b;
}

/** Base name compare ignoring case and a trailing ".exe" / ".dll" / ".sys". */
static int BpSameName(const wchar_t *A, const wchar_t *B)
{
    wchar_t a[BP_ITEM_CHARS], b[BP_ITEM_CHARS];
    size_t la, lb;

    if (!A || !B || !*A || !*B)
        return 0;

    BpCopy(a, BpBaseName(A));
    BpCopy(b, BpBaseName(B));
    la = wcslen(a);
    lb = wcslen(b);

    if (la > 4 && (!_wcsicmp(a + la - 4, L".exe") || !_wcsicmp(a + la - 4, L".dll") || !_wcsicmp(a + la - 4, L".sys")))
        a[la - 4] = 0;

    if (lb > 4 && (!_wcsicmp(b + lb - 4, L".exe") || !_wcsicmp(b + lb - 4, L".dll") || !_wcsicmp(b + lb - 4, L".sys")))
        b[lb - 4] = 0;

    return !_wcsicmp(a, b);
}

static BP_ITEM *BpFindOrAdd(BP_STATE *State, uint32_t EventId, const wchar_t *Key)
{
    uint32_t i;

    for (i = 0; i < State->ItemCount; i++)
    {
        if (State->Items[i].EventId == EventId && !_wcsicmp(State->Items[i].Name, Key))
            return &State->Items[i];
    }

    if (State->ItemCount == State->ItemCapacity)
    {
        uint32_t cap = State->ItemCapacity ? State->ItemCapacity * 2 : 32;
        BP_ITEM *items = (BP_ITEM *)realloc(State->Items, cap * sizeof(BP_ITEM));

        if (!items)
            return NULL;

        State->Items = items;
        State->ItemCapacity = cap;
    }

    memset(&State->Items[State->ItemCount], 0, sizeof(BP_ITEM));
    State->Items[State->ItemCount].EventId = EventId;
    BpCopy(State->Items[State->ItemCount].Name, Key);
    return &State->Items[State->ItemCount++];
}

void BpAddEvent(BP_STATE *State, const BP_EVENT *Event)
{
    State->EventsSeen++;

    if (Event->EventId == 100)
    {
        State->BootCount++;

        if (!State->HasBoot || Event->Time >= State->Boot.Time)
        {
            BP_BOOT *b = &State->Boot;

            memset(b, 0, sizeof(*b));
            b->Time = Event->Time;
            b->BootTimeMs = BpGetUInt(Event, L"BootTime", 0);
            b->MainPathBootTimeMs = BpGetUInt(Event, L"MainPathBootTime", 0);
            b->PostBootTimeMs = BpGetUInt(Event, L"BootPostBootTime", 0);
            b->OsLoaderMs = BpGetUInt(Event, L"OSLoaderDuration", 0);
            b->KernelInitMs = BpGetUInt(Event, L"BootKernelInitTime", 0);
            b->DriverInitMs = BpGetUInt(Event, L"BootDriverInitTime", 0);
            b->DevicesInitMs = BpGetUInt(Event, L"BootDevicesInitTime", 0);
            b->SmssInitMs = BpGetUInt(Event, L"BootSmssInitTime", 0);
            b->CriticalServicesInitMs = BpGetUInt(Event, L"BootCriticalServicesInitTime", 0);
            b->UserProfileMs = BpGetUInt(Event, L"BootUserProfileProcessingTime", 0);
            b->ExplorerInitMs = BpGetUInt(Event, L"BootExplorerInitTime", 0);
            b->UserLogonWaitMs = BpGetUInt(Event, L"UserLogonWaitDuration", 0);
            b->NumStartupApps = BpGetUInt(Event, L"BootNumStartupApps", 0);
            b->IsDegradation = BpGetUInt(Event, L"BootIsDegradation", 0);
            State->HasBoot = 1;
        }

        return;
    }

    if (Event->EventId == 101 || Event->EventId == 102 || Event->EventId == 103 || Event->EventId == 106 || Event->EventId == 108)
    {
        const wchar_t *name = BpGetField(Event, L"Name");
        const wchar_t *friendly = BpGetField(Event, L"FriendlyName");
        const wchar_t *path = BpGetField(Event, L"Path");
        uint32_t total = BpGetUInt(Event, L"TotalTime", 0);
        BP_ITEM *item;

        if (!name || !*name)
            name = friendly;

        if (!name || !*name)
        {
            State->EventsIgnored++;
            return;
        }

        item = BpFindOrAdd(State, Event->EventId, name);

        if (!item)
        {
            State->EventsIgnored++;
            return;
        }

        item->Events++;

        if (Event->Time >= item->Time)
        {
            item->Time = Event->Time;
            item->TotalMs = total;
            item->DegradationMs = BpGetUInt(Event, L"DegradationTime", 0);
            BpCopy(item->Friendly, friendly);
            BpCopy(item->Path, path);
        }

        return;
    }

    State->EventsIgnored++;
}

int BpAddXml(BP_STATE *State, const wchar_t *Xml, BP_EVENT *Scratch)
{
    if (!BpParseEventXml(Xml, Scratch))
    {
        State->EventsIgnored++;
        return 0;
    }

    BpAddEvent(State, Scratch);
    return 1;
}

const BP_ITEM *BpFindItem(const BP_STATE *State, const wchar_t *Name, const wchar_t *FilePath)
{
    const BP_ITEM *best = NULL;
    uint32_t i;

    for (i = 0; i < State->ItemCount; i++)
    {
        const BP_ITEM *it = &State->Items[i];
        int match = 0;

        if (it->EventId != 101 && it->EventId != 103 && it->EventId != 102 && it->EventId != 108)
            continue;

        if (FilePath && *FilePath)
        {
            if (it->Path[0] && BpSameName(it->Path, FilePath))
                match = 1;
            else if (BpSameName(it->Name, FilePath))
                match = 1;
        }

        if (!match && Name && *Name && (BpSameName(it->Name, Name) || (it->Friendly[0] && !_wcsicmp(it->Friendly, Name))))
            match = 1;

        if (match && (!best || it->TotalMs > best->TotalMs))
            best = it;
    }

    return best;
}

int BpRateImpact(uint32_t TotalMs)
{
    if (TotalMs >= 3000)
        return 3;
    if (TotalMs >= 1000)
        return 2;
    return 1;
}
