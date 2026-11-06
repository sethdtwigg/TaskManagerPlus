/*
 * TaskManagerPlus: Information tab.
 *
 * A scrollable page of titled cards (label/value rows) describing the machine: operating
 * system, processor, system, memory (with per-module blocks) and BIOS/motherboard. All data is
 * read locally: registry, GetPerformanceInfo, the process snapshot, and the SMBIOS firmware
 * table (GetSystemFirmwareTable). No WMI, no network.
 *
 * The page is one custom-painted window (double-buffered, vertical scroll bar) plus a filter
 * edit box at the top. Cards are laid out in two columns. Each card header has a thin accent
 * bar on its left and a "Copy" button that puts the card on the clipboard as text. Static
 * values are read once; uptime, memory and counts refresh once a second while the tab is shown.
 */

#include <phapp.h>
#include <phplug.h>
#include <mainwnd.h>
#include <guisup.h>
#include <mainwndp.h>
#include <psapi.h>

#define INFOTAB_CLASS_NAME L"TaskManagerPlusInformationTab"
#define INFOTAB_TIMER_ID 1
#define INFOTAB_TIMER_INTERVAL 1000
#define INFOTAB_ID_FILTER 200
#define INFOTAB_VALUE_LEN 192
#define INFOTAB_LABEL_LEN 48

typedef enum _INFOTAB_KEY
{
    InfoKeyNone,
    InfoKeyUptime,
    InfoKeyBootTime,
    InfoKeyProcesses,
    InfoKeyThreads,
    InfoKeyMemTotal,
    InfoKeyMemAvailable,
    InfoKeyMemUsage,
    InfoKeyCommitted,
    InfoKeyCommitLimit,
    InfoKeyCommitPeak,
    InfoKeyPageSize
} INFOTAB_KEY;

typedef struct _INFOTAB_ROW
{
    INFOTAB_KEY Key;
    BOOLEAN SubHeader; // a block title inside a card (for example one memory module)
    WCHAR Label[INFOTAB_LABEL_LEN];
    WCHAR Value[INFOTAB_VALUE_LEN];
} INFOTAB_ROW, *PINFOTAB_ROW;

typedef struct _INFOTAB_CARD
{
    WCHAR Title[INFOTAB_LABEL_LEN];
    PINFOTAB_ROW Rows;
    ULONG Count;
    ULONG Capacity;
    // Layout, recomputed every paint/scroll-range pass.
    RECT Bounds;
    RECT CopyRect;
    LONG VisibleRows;
    BOOLEAN Hidden;
    BOOLEAN CopiedFlash;
} INFOTAB_CARD, *PINFOTAB_CARD;

enum
{
    CardOs,
    CardProcessor,
    CardSystem,
    CardMemory,
    CardBios,
    CardCount
};

PPH_MAIN_TAB_PAGE PhMwpInformationPage = NULL;

static HWND InfoHostHandle = NULL;
static HWND InfoFilterHandle = NULL;
static BOOLEAN InfoClassRegistered = FALSE;
static BOOLEAN InfoModelBuilt = FALSE;
static INFOTAB_CARD InfoCards[CardCount];
static HFONT InfoBoldFont = NULL;
static HFONT InfoFont = NULL;
static WCHAR InfoFilter[64] = L"";
static LONG InfoScrollPos = 0;
static LONG InfoContentHeight = 0;
static LONG InfoRowHeight = 20;
static LONG InfoHotCopy = -1;

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

static PINFOTAB_ROW PhpInfoAddRow(
    _In_ ULONG CardIndex,
    _In_ INFOTAB_KEY Key,
    _In_ PCWSTR Label
    )
{
    PINFOTAB_CARD card = &InfoCards[CardIndex];
    PINFOTAB_ROW row;

    if (card->Count == card->Capacity)
    {
        card->Capacity = card->Capacity ? card->Capacity * 2 : 16;
        card->Rows = card->Rows ?
            PhReAllocate(card->Rows, card->Capacity * sizeof(INFOTAB_ROW)) :
            PhAllocate(card->Capacity * sizeof(INFOTAB_ROW));
    }

    row = &card->Rows[card->Count++];
    memset(row, 0, sizeof(INFOTAB_ROW));
    row->Key = Key;
    wcsncpy_s(row->Label, INFOTAB_LABEL_LEN, Label, _TRUNCATE);
    return row;
}

static VOID PhpInfoSetValue(
    _Inout_ PINFOTAB_ROW Row,
    _In_opt_ PCWSTR Value
    )
{
    wcsncpy_s(Row->Value, INFOTAB_VALUE_LEN, (Value && Value[0]) ? Value : L"Not available", _TRUNCATE);
}

static VOID PhpInfoSetValueString(
    _Inout_ PINFOTAB_ROW Row,
    _In_opt_ PPH_STRING Value
    )
{
    PhpInfoSetValue(Row, Value ? Value->Buffer : NULL);
    PhClearReference(&Value);
}

static VOID PhpInfoAddText(
    _In_ ULONG CardIndex,
    _In_ PCWSTR Label,
    _In_opt_ PCWSTR Value
    )
{
    PhpInfoSetValue(PhpInfoAddRow(CardIndex, InfoKeyNone, Label), Value);
}

static VOID PhpInfoAddSubHeader(
    _In_ ULONG CardIndex,
    _In_ PCWSTR Title
    )
{
    PINFOTAB_ROW row = PhpInfoAddRow(CardIndex, InfoKeyNone, Title);

    row->SubHeader = TRUE;
}

static BOOLEAN PhpInfoReadRegistry(
    _In_ PCWSTR SubKey,
    _In_ PCWSTR ValueName,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ ULONG Count
    )
{
    DWORD size = Count * sizeof(WCHAR);

    Buffer[0] = UNICODE_NULL;

    return RegGetValueW(
        HKEY_LOCAL_MACHINE,
        SubKey,
        ValueName,
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
        NULL,
        Buffer,
        &size
        ) == ERROR_SUCCESS && Buffer[0];
}

static BOOLEAN PhpInfoReadRegistryDword(
    _In_ PCWSTR SubKey,
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value
    )
{
    DWORD size = sizeof(ULONG);

    *Value = 0;

    return RegGetValueW(
        HKEY_LOCAL_MACHINE,
        SubKey,
        ValueName,
        RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
        NULL,
        Value,
        &size
        ) == ERROR_SUCCESS;
}

static PCWSTR PhpInfoArchitectureName(
    VOID
    )
{
    SYSTEM_INFO info;

    GetNativeSystemInfo(&info);

    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: return L"x64 (AMD64)";
    case PROCESSOR_ARCHITECTURE_ARM64: return L"ARM64";
    case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
    case PROCESSOR_ARCHITECTURE_ARM: return L"ARM";
    }

    return L"Unknown";
}

static PPH_STRING PhpInfoFormatFileTime(
    _In_ PLARGE_INTEGER SystemTime
    )
{
    LARGE_INTEGER local;
    SYSTEMTIME st;

    if (!NT_SUCCESS(RtlSystemTimeToLocalTime(SystemTime, &local)))
        return NULL;

    PhLargeIntegerToSystemTime(&st, &local);
    return PhFormatDateTime(&st);
}

static VOID PhpInfoBuildOs(
    VOID
    )
{
    static const PCWSTR key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    WCHAR name[128], version[64], build[32], owner[128], product[64];
    ULONG ubr = 0;
    ULONG installTime = 0;
    PPH_STRING text;

    PhpInfoReadRegistry(key, L"ProductName", name, RTL_NUMBER_OF(name));
    if (!PhpInfoReadRegistry(key, L"DisplayVersion", version, RTL_NUMBER_OF(version)))
        PhpInfoReadRegistry(key, L"ReleaseId", version, RTL_NUMBER_OF(version));
    PhpInfoReadRegistry(key, L"CurrentBuildNumber", build, RTL_NUMBER_OF(build));
    PhpInfoReadRegistry(key, L"RegisteredOwner", owner, RTL_NUMBER_OF(owner));
    PhpInfoReadRegistry(key, L"ProductId", product, RTL_NUMBER_OF(product));
    PhpInfoReadRegistryDword(key, L"UBR", &ubr);

    // The ProductName value still says "Windows 10" on Windows 11.
    if (_wtoi(build) >= 22000 && wcsncmp(name, L"Windows 10", 10) == 0)
        name[9] = L'1', name[10] = L'1';

    PhpInfoAddText(CardOs, L"Name", name);
    PhpInfoAddText(CardOs, L"Version", version);

    if (build[0])
    {
        text = ubr ? PhFormatString(L"%s.%lu", build, ubr) : PhCreateString(build);
        PhpInfoSetValueString(PhpInfoAddRow(CardOs, InfoKeyNone, L"Build"), text);
    }
    else
    {
        PhpInfoAddText(CardOs, L"Build", NULL);
    }

    PhpInfoAddText(CardOs, L"Architecture", PhpInfoArchitectureName());

    if (PhpInfoReadRegistryDword(key, L"InstallDate", &installTime))
    {
        LARGE_INTEGER time;

        // Seconds since 1970 to a 100ns FILETIME value.
        time.QuadPart = ((LONGLONG)installTime + 11644473600LL) * 10000000LL;
        PhpInfoSetValueString(PhpInfoAddRow(CardOs, InfoKeyNone, L"Install date"), PhpInfoFormatFileTime(&time));
    }
    else
    {
        PhpInfoAddText(CardOs, L"Install date", NULL);
    }

    PhpInfoAddText(CardOs, L"Registered owner", owner);
    PhpInfoAddText(CardOs, L"Product ID", product);
}

static VOID PhpInfoBuildProcessor(
    VOID
    )
{
    static const PCWSTR key = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    WCHAR name[128], vendor[64], identifier[128];
    ULONG mhz = 0;
    PPH_STRING text;

    PhpInfoReadRegistry(key, L"ProcessorNameString", name, RTL_NUMBER_OF(name));
    PhpInfoReadRegistry(key, L"VendorIdentifier", vendor, RTL_NUMBER_OF(vendor));
    PhpInfoReadRegistry(key, L"Identifier", identifier, RTL_NUMBER_OF(identifier));
    PhpInfoReadRegistryDword(key, L"~MHz", &mhz);

    // Names are often padded with leading spaces.
    {
        PWSTR trimmed = name;

        while (*trimmed == L' ')
            trimmed++;

        PhpInfoAddText(CardProcessor, L"Name", trimmed);
    }

    PhpInfoAddText(CardProcessor, L"Vendor", vendor);
    PhpInfoAddText(CardProcessor, L"Architecture", PhpInfoArchitectureName());
    PhpInfoAddText(CardProcessor, L"Identifier", identifier);

    text = PhFormatString(L"%lu", (ULONG)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    PhpInfoSetValueString(PhpInfoAddRow(CardProcessor, InfoKeyNone, L"Logical processors"), text);

    if (mhz)
        text = PhFormatString(L"%.2f GHz", mhz / 1000.0);
    else
        text = NULL;

    PhpInfoSetValueString(PhpInfoAddRow(CardProcessor, InfoKeyNone, L"Speed"), text);
}

static VOID PhpInfoBuildSystem(
    VOID
    )
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1 + 64];
    WCHAR user[256 + 1];
    WCHAR domain[128];
    DWORD size;

    size = RTL_NUMBER_OF(computer);
    if (!GetComputerNameExW(ComputerNameDnsHostname, computer, &size))
        computer[0] = UNICODE_NULL;

    size = RTL_NUMBER_OF(user);
    if (!GetUserNameW(user, &size))
        user[0] = UNICODE_NULL;

    if (!GetEnvironmentVariableW(L"USERDOMAIN", domain, RTL_NUMBER_OF(domain)))
        domain[0] = UNICODE_NULL;

    PhpInfoAddText(CardSystem, L"Computer name", computer);
    PhpInfoAddText(CardSystem, L"User name", user);
    PhpInfoAddText(CardSystem, L"Domain", domain);
    PhpInfoAddRow(CardSystem, InfoKeyUptime, L"Uptime");
    PhpInfoAddRow(CardSystem, InfoKeyBootTime, L"Boot time");
    PhpInfoAddRow(CardSystem, InfoKeyProcesses, L"Processes");
    PhpInfoAddRow(CardSystem, InfoKeyThreads, L"Threads");
}

// --- SMBIOS ----------------------------------------------------------------------------------

typedef struct _INFOTAB_SMBIOS_HEADER
{
    UCHAR Type;
    UCHAR Length;
    USHORT Handle;
} INFOTAB_SMBIOS_HEADER, *PINFOTAB_SMBIOS_HEADER;

static PCSTR PhpInfoSmbiosString(
    _In_ PUCHAR Structure,
    _In_ PUCHAR End,
    _In_ ULONG Index
    )
{
    PUCHAR strings;
    ULONG i;

    if (Index == 0 || Structure + 1 >= End)
        return NULL;

    strings = Structure + Structure[1];

    for (i = 1; strings < End && *strings; i++)
    {
        PCSTR s = (PCSTR)strings;
        SIZE_T length = strnlen_s(s, (SIZE_T)(End - strings));

        if (strings + length >= End)
            return NULL;

        if (i == Index)
            return s;

        strings += length + 1;
    }

    return NULL;
}

static PPH_STRING PhpInfoSmbiosText(
    _In_ PUCHAR Structure,
    _In_ PUCHAR End,
    _In_ ULONG Offset
    )
{
    PCSTR s;
    PPH_STRING string;
    PWSTR p;

    if (Offset >= Structure[1])
        return NULL;

    s = PhpInfoSmbiosString(Structure, End, Structure[Offset]);

    if (!s || !s[0])
        return NULL;

    string = PhConvertUtf8ToUtf16Ex((PCCH)s, strlen(s));

    if (!string)
        return NULL;

    // Trim trailing spaces that firmware likes to pad strings with.
    p = string->Buffer + string->Length / sizeof(WCHAR);
    while (p > string->Buffer && p[-1] == L' ')
        p--;
    if ((SIZE_T)(p - string->Buffer) != string->Length / sizeof(WCHAR))
    {
        PPH_STRING trimmed = PhCreateStringEx(string->Buffer, (SIZE_T)(p - string->Buffer) * sizeof(WCHAR));
        PhDereferenceObject(string);
        string = trimmed;
    }

    if (string->Length == 0 || PhEqualString2(string, L"To Be Filled By O.E.M.", TRUE) || PhEqualString2(string, L"Default string", TRUE))
    {
        PhDereferenceObject(string);
        return NULL;
    }

    return string;
}

static PCWSTR PhpInfoMemoryType(
    _In_ UCHAR Type
    )
{
    switch (Type)
    {
    case 0x12: return L"DDR";
    case 0x13: return L"DDR2";
    case 0x14: return L"DDR2 FB-DIMM";
    case 0x18: return L"DDR3";
    case 0x1A: return L"DDR4";
    case 0x1B: return L"LPDDR";
    case 0x1C: return L"LPDDR2";
    case 0x1D: return L"LPDDR3";
    case 0x1E: return L"LPDDR4";
    case 0x1F: return L"Logical non-volatile";
    case 0x20: return L"HBM";
    case 0x21: return L"HBM2";
    case 0x22: return L"DDR5";
    case 0x23: return L"LPDDR5";
    }

    return NULL;
}

static PCWSTR PhpInfoFormFactor(
    _In_ UCHAR Type
    )
{
    switch (Type)
    {
    case 0x03: return L"SIMM";
    case 0x04: return L"SIP";
    case 0x05: return L"Chip";
    case 0x06: return L"DIP";
    case 0x07: return L"ZIP";
    case 0x08: return L"Proprietary card";
    case 0x09: return L"DIMM";
    case 0x0A: return L"TSOP";
    case 0x0B: return L"Row of chips";
    case 0x0C: return L"RIMM";
    case 0x0D: return L"SODIMM";
    case 0x0E: return L"SRIMM";
    case 0x0F: return L"FB-DIMM";
    case 0x10: return L"Die";
    }

    return NULL;
}

static VOID PhpInfoAddSmbiosText(
    _In_ ULONG CardIndex,
    _In_ PCWSTR Label,
    _In_opt_ PPH_STRING Value
    )
{
    PhpInfoSetValueString(PhpInfoAddRow(CardIndex, InfoKeyNone, Label), Value);
}

static VOID PhpInfoBuildFirmware(
    VOID
    )
{
    PUCHAR table = NULL;
    PUCHAR data;
    PUCHAR end;
    ULONG size;
    ULONG got;
    ULONG moduleNumber = 0;
    BOOLEAN haveBios = FALSE;
    BOOLEAN haveSystem = FALSE;
    BOOLEAN haveBoard = FALSE;
    PPH_STRING biosVendor = NULL, biosVersion = NULL, biosDate = NULL;
    PPH_STRING sysMaker = NULL, sysModel = NULL;
    PPH_STRING boardMaker = NULL, boardModel = NULL, boardVersion = NULL;
    PPH_STRING smbiosVersion = NULL;

    size = GetSystemFirmwareTable('RSMB', 0, NULL, 0);

    if (size > 8 && size < 8 * 1024 * 1024)
    {
        table = PhAllocate(size);
        got = GetSystemFirmwareTable('RSMB', 0, table, size);

        if (got == 0 || got > size)
        {
            PhFree(table);
            table = NULL;
        }
        else
        {
            size = got;
        }
    }

    if (table)
    {
        // RawSMBIOSData: BYTE Used20CallingMethod, Major, Minor, DmiRevision; DWORD Length; data.
        ULONG length = *(PULONG)(table + 4);

        smbiosVersion = PhFormatString(L"%u.%u", table[1], table[2]);
        data = table + 8;
        end = data + min(length, size - 8);

        while (data + sizeof(INFOTAB_SMBIOS_HEADER) <= end)
        {
            PINFOTAB_SMBIOS_HEADER header = (PINFOTAB_SMBIOS_HEADER)data;
            PUCHAR next;

            if (header->Length < sizeof(INFOTAB_SMBIOS_HEADER) || data + header->Length > end)
                break;

            // The string set ends with a double NUL.
            next = data + header->Length;
            while (next + 1 < end && !(next[0] == 0 && next[1] == 0))
                next++;
            next += 2;

            if (header->Type == 127)
                break;

            if (header->Type == 0 && !haveBios)
            {
                haveBios = TRUE;
                biosVendor = PhpInfoSmbiosText(data, end, 4);
                biosVersion = PhpInfoSmbiosText(data, end, 5);
                biosDate = PhpInfoSmbiosText(data, end, 8);
            }
            else if (header->Type == 1 && !haveSystem)
            {
                haveSystem = TRUE;
                sysMaker = PhpInfoSmbiosText(data, end, 4);
                sysModel = PhpInfoSmbiosText(data, end, 5);
            }
            else if (header->Type == 2 && !haveBoard)
            {
                haveBoard = TRUE;
                boardMaker = PhpInfoSmbiosText(data, end, 4);
                boardModel = PhpInfoSmbiosText(data, end, 5);
                boardVersion = PhpInfoSmbiosText(data, end, 6);
            }
            else if (header->Type == 17 && header->Length >= 0x15)
            {
                // Memory device.
                ULONG sizeField = *(PUSHORT)(data + 0x0C);
                ULONG64 bytes = 0;
                ULONG speed;
                PCWSTR text;
                PPH_STRING string;

                if (sizeField == 0xFFFF)
                {
                    bytes = 0;
                }
                else if (sizeField == 0x7FFF && header->Length >= 0x20)
                {
                    bytes = (ULONG64)(*(PULONG)(data + 0x1C) & 0x7FFFFFFF) * 1024 * 1024;
                }
                else if (sizeField & 0x8000)
                {
                    bytes = (ULONG64)(sizeField & 0x7FFF) * 1024;
                }
                else
                {
                    bytes = (ULONG64)sizeField * 1024 * 1024;
                }

                if (bytes != 0) // skip empty slots
                {
                    moduleNumber++;

                    string = PhFormatString(L"Module %lu", moduleNumber);
                    PhpInfoAddSubHeader(CardMemory, string->Buffer);
                    PhDereferenceObject(string);

                    string = PhpInfoSmbiosText(data, end, 0x10);
                    if (string)
                    {
                        PPH_STRING bank = PhpInfoSmbiosText(data, end, 0x11);

                        if (bank)
                        {
                            PPH_STRING both = PhFormatString(L"%s (%s)", string->Buffer, bank->Buffer);
                            PhDereferenceObject(string);
                            PhDereferenceObject(bank);
                            string = both;
                        }
                    }
                    PhpInfoAddSmbiosText(CardMemory, L"Slot", string);

                    PhpInfoAddSmbiosText(CardMemory, L"Capacity", PhFormatSize(bytes, ULONG_MAX));

                    speed = 0;
                    if (header->Length >= 0x22)
                        speed = *(PUSHORT)(data + 0x20); // configured speed
                    if (speed == 0 || speed == 0xFFFF)
                        speed = *(PUSHORT)(data + 0x15);

                    if (speed && speed != 0xFFFF)
                        PhpInfoAddSmbiosText(CardMemory, L"Speed", PhFormatString(L"%lu MT/s", speed));
                    else
                        PhpInfoAddSmbiosText(CardMemory, L"Speed", NULL);

                    text = PhpInfoMemoryType(data[0x12]);
                    PhpInfoAddText(CardMemory, L"Type", text);

                    text = PhpInfoFormFactor(data[0x0E]);
                    PhpInfoAddText(CardMemory, L"Form factor", text);

                    PhpInfoAddSmbiosText(CardMemory, L"Manufacturer", header->Length > 0x17 ? PhpInfoSmbiosText(data, end, 0x17) : NULL);
                    PhpInfoAddSmbiosText(CardMemory, L"Part number", header->Length > 0x1A ? PhpInfoSmbiosText(data, end, 0x1A) : NULL);
                }
            }

            data = next;
        }

        PhFree(table);
    }

    if (moduleNumber == 0)
    {
        PhpInfoAddSubHeader(CardMemory, L"Modules");
        PhpInfoAddText(CardMemory, L"Details", L"Not reported by the firmware");
    }

    PhpInfoAddSmbiosText(CardBios, L"BIOS vendor", biosVendor);
    PhpInfoAddSmbiosText(CardBios, L"BIOS version", biosVersion);
    PhpInfoAddSmbiosText(CardBios, L"BIOS date", biosDate);
    PhpInfoAddSmbiosText(CardBios, L"SMBIOS version", smbiosVersion);
    PhpInfoAddSmbiosText(CardBios, L"System maker", sysMaker);
    PhpInfoAddSmbiosText(CardBios, L"System model", sysModel);
    PhpInfoAddSmbiosText(CardBios, L"Board maker", boardMaker);
    PhpInfoAddSmbiosText(CardBios, L"Board model", boardModel);
    PhpInfoAddSmbiosText(CardBios, L"Board version", boardVersion);
}

static VOID PhpInfoBuildModel(
    VOID
    )
{
    ULONG i;

    if (InfoModelBuilt)
        return;

    memset(InfoCards, 0, sizeof(InfoCards));
    wcscpy_s(InfoCards[CardOs].Title, INFOTAB_LABEL_LEN, L"Operating System");
    wcscpy_s(InfoCards[CardProcessor].Title, INFOTAB_LABEL_LEN, L"Processor");
    wcscpy_s(InfoCards[CardSystem].Title, INFOTAB_LABEL_LEN, L"System");
    wcscpy_s(InfoCards[CardMemory].Title, INFOTAB_LABEL_LEN, L"Memory");
    wcscpy_s(InfoCards[CardBios].Title, INFOTAB_LABEL_LEN, L"BIOS / Motherboard");

    PhpInfoBuildOs();
    PhpInfoBuildProcessor();
    PhpInfoBuildSystem();

    // Memory: the live rows first, then the per-module blocks.
    PhpInfoAddRow(CardMemory, InfoKeyMemTotal, L"Total physical");
    PhpInfoAddRow(CardMemory, InfoKeyMemAvailable, L"Available");
    PhpInfoAddRow(CardMemory, InfoKeyMemUsage, L"Usage");
    PhpInfoAddRow(CardMemory, InfoKeyCommitted, L"Committed");
    PhpInfoAddRow(CardMemory, InfoKeyCommitLimit, L"Commit limit");
    PhpInfoAddRow(CardMemory, InfoKeyCommitPeak, L"Peak committed");
    PhpInfoAddRow(CardMemory, InfoKeyPageSize, L"Page size");

    PhpInfoBuildFirmware();

    for (i = 0; i < CardCount; i++)
        InfoCards[i].CopiedFlash = FALSE;

    InfoModelBuilt = TRUE;
}

static VOID PhpInfoFormatDuration(
    _In_ ULONG64 Milliseconds,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ ULONG Count
    )
{
    ULONG64 seconds = Milliseconds / 1000;
    ULONG days = (ULONG)(seconds / 86400);
    ULONG hours = (ULONG)((seconds / 3600) % 24);
    ULONG minutes = (ULONG)((seconds / 60) % 60);
    ULONG secs = (ULONG)(seconds % 60);

    if (days)
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lu days, %lu:%02lu:%02lu", days, hours, minutes, secs);
    else
        _snwprintf_s(Buffer, Count, _TRUNCATE, L"%lu:%02lu:%02lu", hours, minutes, secs);
}

/**
 * Refreshes the rows whose values change over time: uptime, memory and the counts.
 */
static VOID PhpInfoRefreshDynamic(
    VOID
    )
{
    PERFORMANCE_INFORMATION perf;
    BOOLEAN havePerf;
    ULONG processCount = 0;
    ULONG threadCount = 0;
    BOOLEAN haveCounts = FALSE;
    ULONG64 tick;
    PVOID processes = NULL;
    ULONG c;
    ULONG r;

    if (!InfoModelBuilt)
        return;

    memset(&perf, 0, sizeof(perf));
    perf.cb = sizeof(perf);
    havePerf = GetPerformanceInfo(&perf, sizeof(perf)) && perf.PageSize != 0;

    // Count from a real process snapshot so a failing counter cannot show 0.
    if (NT_SUCCESS(PhEnumProcesses(&processes)))
    {
        PSYSTEM_PROCESS_INFORMATION process = PH_FIRST_PROCESS(processes);

        while (process)
        {
            processCount++;
            threadCount += process->NumberOfThreads;
            process = PH_NEXT_PROCESS(process);
        }

        haveCounts = processCount != 0;
        PhFree(processes);
    }

    if (!haveCounts && havePerf)
    {
        processCount = perf.ProcessCount;
        threadCount = perf.ThreadCount;
        haveCounts = TRUE;
    }

    tick = GetTickCount64();

    for (c = 0; c < CardCount; c++)
    {
        for (r = 0; r < InfoCards[c].Count; r++)
        {
            PINFOTAB_ROW row = &InfoCards[c].Rows[r];
            PPH_STRING text = NULL;
            WCHAR buffer[96];

            switch (row->Key)
            {
            case InfoKeyUptime:
                PhpInfoFormatDuration(tick, buffer, RTL_NUMBER_OF(buffer));
                PhpInfoSetValue(row, buffer);
                break;
            case InfoKeyBootTime:
                {
                    LARGE_INTEGER now;
                    LARGE_INTEGER boot;

                    PhQuerySystemTime(&now);
                    boot.QuadPart = now.QuadPart - (LONGLONG)tick * 10000;
                    PhpInfoSetValueString(row, PhpInfoFormatFileTime(&boot));
                }
                break;
            case InfoKeyProcesses:
                if (haveCounts)
                    text = PhFormatString(L"%lu", processCount);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyThreads:
                if (haveCounts)
                    text = PhFormatString(L"%lu", threadCount);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyMemTotal:
                if (havePerf)
                    text = PhFormatSize((ULONG64)perf.PhysicalTotal * perf.PageSize, ULONG_MAX);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyMemAvailable:
                if (havePerf)
                    text = PhFormatSize((ULONG64)perf.PhysicalAvailable * perf.PageSize, ULONG_MAX);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyMemUsage:
                if (havePerf && perf.PhysicalTotal)
                {
                    ULONG64 used = perf.PhysicalTotal - min(perf.PhysicalAvailable, perf.PhysicalTotal);

                    text = PhFormatString(L"%lu%%", (ULONG)(used * 100 / perf.PhysicalTotal));
                }
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyCommitted:
                if (havePerf)
                    text = PhFormatSize((ULONG64)perf.CommitTotal * perf.PageSize, ULONG_MAX);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyCommitLimit:
                if (havePerf)
                    text = PhFormatSize((ULONG64)perf.CommitLimit * perf.PageSize, ULONG_MAX);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyCommitPeak:
                if (havePerf)
                    text = PhFormatSize((ULONG64)perf.CommitPeak * perf.PageSize, ULONG_MAX);
                PhpInfoSetValueString(row, text);
                break;
            case InfoKeyPageSize:
                if (havePerf)
                    text = PhFormatSize(perf.PageSize, ULONG_MAX);
                PhpInfoSetValueString(row, text);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Filtering, layout, painting
// ---------------------------------------------------------------------------------------------

static BOOLEAN PhpInfoContainsNoCase(
    _In_ PCWSTR Text,
    _In_ PCWSTR Needle
    )
{
    SIZE_T needleLength = wcslen(Needle);
    SIZE_T i;

    if (needleLength == 0)
        return TRUE;

    for (i = 0; Text[i]; i++)
    {
        SIZE_T j = 0;

        while (j < needleLength && Text[i + j] && towlower(Text[i + j]) == towlower(Needle[j]))
            j++;

        if (j == needleLength)
            return TRUE;
    }

    return FALSE;
}

static BOOLEAN PhpInfoRowMatches(
    _In_ PINFOTAB_CARD Card,
    _In_ ULONG Index
    )
{
    PINFOTAB_ROW row = &Card->Rows[Index];

    if (!InfoFilter[0])
        return TRUE;

    if (PhpInfoContainsNoCase(Card->Title, InfoFilter))
        return TRUE;

    if (row->SubHeader)
    {
        // A block title stays when any row of its block matches.
        ULONG i;

        for (i = Index + 1; i < Card->Count && !Card->Rows[i].SubHeader; i++)
        {
            if (PhpInfoContainsNoCase(Card->Rows[i].Label, InfoFilter) ||
                PhpInfoContainsNoCase(Card->Rows[i].Value, InfoFilter))
                return TRUE;
        }

        return PhpInfoContainsNoCase(row->Label, InfoFilter);
    }

    return PhpInfoContainsNoCase(row->Label, InfoFilter) || PhpInfoContainsNoCase(row->Value, InfoFilter);
}

static COLORREF PhpInfoBackColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowBackgroundColor : GetSysColor(COLOR_BTNFACE);
}

static COLORREF PhpInfoCardColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowBackground2Color : GetSysColor(COLOR_WINDOW);
}

static COLORREF PhpInfoTextColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowTextColor : GetSysColor(COLOR_WINDOWTEXT);
}

static COLORREF PhpInfoMutedColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowDisabledTextColor : GetSysColor(COLOR_GRAYTEXT);
}

static COLORREF PhpInfoBorderColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowBorderColor : GetSysColor(COLOR_3DLIGHT);
}

static COLORREF PhpInfoAccentColor(
    VOID
    )
{
    return RGB(0x1E, 0x78, 0xD2);
}

static LONG PhpInfoFilterHeight(
    VOID
    )
{
    return InfoRowHeight + PhScaleToDisplay(14, PhGetWindowDpi(InfoHostHandle));
}

/**
 * Lays out the cards for the given client width. Fills each card's Bounds in content
 * coordinates (before scrolling) and returns the content height.
 */
static LONG PhpInfoLayout(
    _In_ LONG ClientWidth
    )
{
    ULONG dpi = PhGetWindowDpi(InfoHostHandle);
    LONG margin = PhScaleToDisplay(12, dpi);
    LONG gap = PhScaleToDisplay(12, dpi);
    LONG headerHeight = InfoRowHeight + PhScaleToDisplay(10, dpi);
    LONG pad = PhScaleToDisplay(8, dpi);
    LONG columnWidth = (ClientWidth - margin * 2 - gap) / 2;
    LONG columnY[2];
    LONG sideBySide = ClientWidth >= PhScaleToDisplay(640, dpi);
    ULONG c;
    ULONG r;

    if (!sideBySide)
        columnWidth = ClientWidth - margin * 2;

    columnY[0] = columnY[1] = margin;

    for (c = 0; c < CardCount; c++)
    {
        PINFOTAB_CARD card = &InfoCards[c];
        // Left column: OS, Processor, System. Right column: Memory, BIOS.
        ULONG column = sideBySide ? (c <= CardSystem ? 0 : 1) : 0;
        LONG y;
        LONG x;
        LONG height;
        LONG buttonWidth = PhScaleToDisplay(56, dpi);

        card->VisibleRows = 0;

        for (r = 0; r < card->Count; r++)
        {
            if (PhpInfoRowMatches(card, r))
                card->VisibleRows++;
        }

        card->Hidden = InfoFilter[0] && card->VisibleRows == 0;

        if (card->Hidden)
        {
            SetRectEmpty(&card->Bounds);
            SetRectEmpty(&card->CopyRect);
            continue;
        }

        x = margin + (LONG)column * (columnWidth + gap);
        y = columnY[column];
        height = headerHeight + pad + card->VisibleRows * InfoRowHeight + pad;

        card->Bounds.left = x;
        card->Bounds.top = y;
        card->Bounds.right = x + columnWidth;
        card->Bounds.bottom = y + height;

        card->CopyRect.right = card->Bounds.right - pad;
        card->CopyRect.left = card->CopyRect.right - buttonWidth;
        card->CopyRect.top = y + PhScaleToDisplay(4, dpi);
        card->CopyRect.bottom = y + headerHeight - PhScaleToDisplay(4, dpi);

        columnY[column] = y + height + gap;
    }

    return max(columnY[0], columnY[1]);
}

static VOID PhpInfoUpdateScroll(
    VOID
    )
{
    RECT client;
    SCROLLINFO si;
    LONG viewHeight;
    LONG top;

    if (!InfoHostHandle || !GetClientRect(InfoHostHandle, &client))
        return;

    top = PhpInfoFilterHeight();
    viewHeight = max(client.bottom - top, 0);

    // The scroll bar may take width away; lay out against the current client width and
    // let a second pass settle it.
    InfoContentHeight = PhpInfoLayout(client.right);

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = max(InfoContentHeight, 1) - 1;
    si.nPage = viewHeight;

    InfoScrollPos = max(0, min(InfoScrollPos, max(InfoContentHeight - viewHeight, 0)));
    si.nPos = InfoScrollPos;
    SetScrollInfo(InfoHostHandle, SB_VERT, &si, TRUE);

    // Scroll bar visibility can change the client width.
    {
        RECT after;

        if (GetClientRect(InfoHostHandle, &after) && after.right != client.right)
            InfoContentHeight = PhpInfoLayout(after.right);
    }
}

static VOID PhpInfoFiltered(
    VOID
    )
{
    InfoScrollPos = 0;
    PhpInfoUpdateScroll();
    InvalidateRect(InfoHostHandle, NULL, FALSE);
}

static VOID PhpInfoLayoutChildren(
    VOID
    )
{
    RECT client;
    ULONG dpi;
    LONG margin;

    if (!InfoHostHandle || !InfoFilterHandle || !GetClientRect(InfoHostHandle, &client))
        return;

    dpi = PhGetWindowDpi(InfoHostHandle);
    margin = PhScaleToDisplay(12, dpi);

    SetWindowPos(
        InfoFilterHandle,
        NULL,
        margin,
        PhScaleToDisplay(6, dpi),
        max(min(client.right - margin * 2, PhScaleToDisplay(320, dpi)), 40),
        InfoRowHeight + PhScaleToDisplay(4, dpi),
        SWP_NOACTIVATE | SWP_NOZORDER
        );
}

static VOID PhpInfoUpdateFonts(
    _In_opt_ HFONT Font
    )
{
    LOGFONT logFont;
    HDC hdc;
    HFONT oldFont;
    TEXTMETRIC tm;

    InfoFont = Font ? Font : PhTreeWindowFont;

    if (!InfoFont)
        return;

    if (InfoBoldFont)
    {
        DeleteObject(InfoBoldFont);
        InfoBoldFont = NULL;
    }

    if (GetObject(InfoFont, sizeof(logFont), &logFont))
    {
        logFont.lfWeight = FW_SEMIBOLD;
        InfoBoldFont = CreateFontIndirect(&logFont);
    }

    hdc = GetDC(InfoHostHandle);

    if (hdc)
    {
        oldFont = SelectObject(hdc, InfoFont);

        if (GetTextMetrics(hdc, &tm))
            InfoRowHeight = tm.tmHeight + PhScaleToDisplay(8, PhGetWindowDpi(InfoHostHandle));

        SelectObject(hdc, oldFont);
        ReleaseDC(InfoHostHandle, hdc);
    }

    if (InfoFilterHandle)
        SetWindowFont(InfoFilterHandle, InfoFont, TRUE);

    PhpInfoLayoutChildren();
    PhpInfoUpdateScroll();
}

static VOID PhpInfoFillRect(
    _In_ HDC Hdc,
    _In_ const RECT *Rect,
    _In_ COLORREF Color
    )
{
    SetDCBrushColor(Hdc, Color);
    FillRect(Hdc, Rect, PhGetStockBrush(DC_BRUSH));
}

static VOID PhpInfoPaintCard(
    _In_ HDC Hdc,
    _In_ ULONG CardIndex,
    _In_ LONG OffsetY,
    _In_ ULONG Dpi
    )
{
    PINFOTAB_CARD card = &InfoCards[CardIndex];
    RECT bounds = card->Bounds;
    RECT rect;
    LONG headerHeight = InfoRowHeight + PhScaleToDisplay(10, Dpi);
    LONG pad = PhScaleToDisplay(8, Dpi);
    LONG inner = PhScaleToDisplay(12, Dpi);
    LONG labelWidth;
    LONG y;
    ULONG r;
    COLORREF cardColor = PhpInfoCardColor();

    OffsetRect(&bounds, 0, OffsetY);

    PhpInfoFillRect(Hdc, &bounds, cardColor);

    // Card outline.
    SetDCBrushColor(Hdc, PhpInfoBorderColor());
    FrameRect(Hdc, &bounds, PhGetStockBrush(DC_BRUSH));

    // Accent bar on the left of the header.
    rect.left = bounds.left;
    rect.top = bounds.top + PhScaleToDisplay(6, Dpi);
    rect.right = bounds.left + PhScaleToDisplay(3, Dpi);
    rect.bottom = bounds.top + headerHeight - PhScaleToDisplay(6, Dpi);
    PhpInfoFillRect(Hdc, &rect, PhpInfoAccentColor());

    // Title.
    SetBkMode(Hdc, TRANSPARENT);
    SelectObject(Hdc, InfoBoldFont ? InfoBoldFont : InfoFont);
    SetTextColor(Hdc, PhpInfoTextColor());
    rect.left = bounds.left + inner;
    rect.top = bounds.top;
    rect.right = card->CopyRect.left;
    rect.bottom = bounds.top + headerHeight;
    DrawText(Hdc, card->Title, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Copy button.
    {
        RECT button = card->CopyRect;
        BOOLEAN hot = InfoHotCopy == (LONG)CardIndex;

        OffsetRect(&button, 0, OffsetY);
        SelectObject(Hdc, InfoFont);

        if (hot)
        {
            PhpInfoFillRect(Hdc, &button, PhEnableThemeSupport ? PhThemeWindowHighlightColor : GetSysColor(COLOR_BTNFACE));
        }

        SetDCBrushColor(Hdc, PhpInfoBorderColor());
        FrameRect(Hdc, &button, PhGetStockBrush(DC_BRUSH));
        SetTextColor(Hdc, hot ? PhpInfoTextColor() : PhpInfoMutedColor());
        DrawText(Hdc, card->CopiedFlash ? L"Copied" : L"Copy", -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Header divider.
    rect.left = bounds.left + 1;
    rect.right = bounds.right - 1;
    rect.top = bounds.top + headerHeight;
    rect.bottom = rect.top + 1;
    PhpInfoFillRect(Hdc, &rect, PhpInfoBorderColor());

    // Rows.
    y = bounds.top + headerHeight + pad;
    labelWidth = (bounds.right - bounds.left) * 2 / 5;

    for (r = 0; r < card->Count; r++)
    {
        PINFOTAB_ROW row = &card->Rows[r];

        if (!PhpInfoRowMatches(card, r))
            continue;

        if (row->SubHeader)
        {
            rect.left = bounds.left + inner;
            rect.right = bounds.right - inner;
            rect.top = y;
            rect.bottom = y + InfoRowHeight;
            SelectObject(Hdc, InfoBoldFont ? InfoBoldFont : InfoFont);
            SetTextColor(Hdc, PhpInfoTextColor());
            DrawText(Hdc, row->Label, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Thin rule under the block title.
            rect.top = y + InfoRowHeight - 1;
            rect.bottom = rect.top + 1;
            PhpInfoFillRect(Hdc, &rect, PhpInfoBorderColor());
        }
        else
        {
            SelectObject(Hdc, InfoFont);

            rect.left = bounds.left + inner;
            rect.right = bounds.left + labelWidth;
            rect.top = y;
            rect.bottom = y + InfoRowHeight;
            SetTextColor(Hdc, PhpInfoMutedColor());
            DrawText(Hdc, row->Label, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            rect.left = bounds.left + labelWidth;
            rect.right = bounds.right - inner;
            SetTextColor(Hdc, PhpInfoTextColor());
            DrawText(Hdc, row->Value, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        y += InfoRowHeight;
    }
}

static VOID PhpInfoPaint(
    _In_ HWND WindowHandle
    )
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(WindowHandle, &ps);
    RECT client;
    HDC memDc;
    HBITMAP bitmap;
    HBITMAP oldBitmap;
    HFONT oldFont;
    ULONG dpi;
    LONG top;
    HRGN clip;
    ULONG c;

    GetClientRect(WindowHandle, &client);

    if (client.right <= 0 || client.bottom <= 0)
    {
        EndPaint(WindowHandle, &ps);
        return;
    }

    dpi = PhGetWindowDpi(WindowHandle);
    top = PhpInfoFilterHeight();

    memDc = CreateCompatibleDC(hdc);
    bitmap = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    oldBitmap = SelectObject(memDc, bitmap);
    oldFont = SelectObject(memDc, InfoFont ? InfoFont : PhTreeWindowFont);

    PhpInfoFillRect(memDc, &client, PhpInfoBackColor());

    // Cards scroll under the filter strip.
    clip = CreateRectRgn(0, top, client.right, client.bottom);
    SelectClipRgn(memDc, clip);

    for (c = 0; c < CardCount; c++)
    {
        if (!InfoCards[c].Hidden && InfoCards[c].Bounds.bottom > InfoCards[c].Bounds.top)
            PhpInfoPaintCard(memDc, c, top - InfoScrollPos, dpi);
    }

    SelectClipRgn(memDc, NULL);
    DeleteObject(clip);

    BitBlt(hdc, 0, 0, client.right, client.bottom, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldFont);
    SelectObject(memDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    EndPaint(WindowHandle, &ps);
}

static LONG PhpInfoHitCopy(
    _In_ LONG X,
    _In_ LONG Y
    )
{
    LONG top = PhpInfoFilterHeight();
    POINT point;
    ULONG c;

    point.x = X;
    point.y = Y - top + InfoScrollPos;

    if (Y < top)
        return -1;

    for (c = 0; c < CardCount; c++)
    {
        if (!InfoCards[c].Hidden && PtInRect(&InfoCards[c].CopyRect, point))
            return (LONG)c;
    }

    return -1;
}

static VOID PhpInfoCopyCard(
    _In_ ULONG CardIndex
    )
{
    PINFOTAB_CARD card = &InfoCards[CardIndex];
    PH_STRING_BUILDER sb;
    PPH_STRING text;
    ULONG r;

    PhInitializeStringBuilder(&sb, 256);
    PhAppendStringBuilder2(&sb, card->Title);
    PhAppendStringBuilder2(&sb, L"\r\n");

    for (r = 0; r < card->Count; r++)
    {
        PINFOTAB_ROW row = &card->Rows[r];

        if (row->SubHeader)
        {
            PhAppendStringBuilder2(&sb, L"\r\n");
            PhAppendStringBuilder2(&sb, row->Label);
            PhAppendStringBuilder2(&sb, L"\r\n");
        }
        else
        {
            PhAppendStringBuilder2(&sb, row->Label);
            PhAppendStringBuilder2(&sb, L": ");
            PhAppendStringBuilder2(&sb, row->Value);
            PhAppendStringBuilder2(&sb, L"\r\n");
        }
    }

    text = PhFinalStringBuilderString(&sb);
    PhSetClipboardString(InfoHostHandle, &text->sr);
    PhDereferenceObject(text);

    card->CopiedFlash = TRUE;
}

static VOID PhpInfoScrollTo(
    _In_ LONG NewPos
    )
{
    RECT client;
    LONG maxPos;

    GetClientRect(InfoHostHandle, &client);
    maxPos = max(InfoContentHeight - (client.bottom - PhpInfoFilterHeight()), 0);
    NewPos = max(0, min(NewPos, maxPos));

    if (NewPos == InfoScrollPos)
        return;

    InfoScrollPos = NewPos;
    SetScrollPos(InfoHostHandle, SB_VERT, InfoScrollPos, TRUE);
    InvalidateRect(InfoHostHandle, NULL, FALSE);
}

static VOID PhpInfoRefreshNow(
    VOID
    )
{
    ULONG c;

    for (c = 0; c < CardCount; c++)
        InfoCards[c].CopiedFlash = FALSE;

    PhpInfoBuildModel();
    PhpInfoRefreshDynamic();
    PhpInfoUpdateScroll();
    InvalidateRect(InfoHostHandle, NULL, FALSE);
}

static LRESULT CALLBACK PhpInfoWindowProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_SIZE:
        {
            PhpInfoLayoutChildren();
            PhpInfoUpdateScroll();
            InvalidateRect(WindowHandle, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        PhpInfoPaint(WindowHandle);
        return 0;
    case WM_VSCROLL:
        {
            SCROLLINFO si;
            LONG pos = InfoScrollPos;

            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(WindowHandle, SB_VERT, &si);

            switch (LOWORD(wParam))
            {
            case SB_LINEUP: pos -= InfoRowHeight; break;
            case SB_LINEDOWN: pos += InfoRowHeight; break;
            case SB_PAGEUP: pos -= (LONG)si.nPage; break;
            case SB_PAGEDOWN: pos += (LONG)si.nPage; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = si.nMax; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }

            PhpInfoScrollTo(pos);
        }
        return 0;
    case WM_MOUSEWHEEL:
        {
            LONG delta = GET_WHEEL_DELTA_WPARAM(wParam);

            PhpInfoScrollTo(InfoScrollPos - delta * (InfoRowHeight * 3) / WHEEL_DELTA);
        }
        return 0;
    case WM_MOUSEMOVE:
        {
            LONG hot = PhpInfoHitCopy(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

            if (hot != InfoHotCopy)
            {
                TRACKMOUSEEVENT track;

                InfoHotCopy = hot;
                InvalidateRect(WindowHandle, NULL, FALSE);

                memset(&track, 0, sizeof(track));
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = WindowHandle;
                TrackMouseEvent(&track);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        {
            if (InfoHotCopy != -1)
            {
                InfoHotCopy = -1;
                InvalidateRect(WindowHandle, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        {
            LONG hit = PhpInfoHitCopy(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

            if (hit >= 0)
            {
                PhpInfoCopyCard((ULONG)hit);
                InvalidateRect(WindowHandle, NULL, FALSE);
            }
        }
        return 0;
    case WM_COMMAND:
        {
            if (LOWORD(wParam) == INFOTAB_ID_FILTER && HIWORD(wParam) == EN_CHANGE)
            {
                GetWindowText(InfoFilterHandle, InfoFilter, RTL_NUMBER_OF(InfoFilter));
                PhpInfoFiltered();
            }
        }
        return 0;
    case WM_TIMER:
        {
            if (wParam == INFOTAB_TIMER_ID)
                PhpInfoRefreshNow();
        }
        return 0;
    case WM_CTLCOLOREDIT:
        {
            SetTextColor((HDC)wParam, PhpInfoTextColor());
            SetBkColor((HDC)wParam, PhEnableThemeSupport ? PhThemeWindowEditColor : GetSysColor(COLOR_WINDOW));
            SetDCBrushColor((HDC)wParam, PhEnableThemeSupport ? PhThemeWindowEditColor : GetSysColor(COLOR_WINDOW));
        }
        return (LRESULT)PhGetStockBrush(DC_BRUSH);
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        InvalidateRect(WindowHandle, NULL, FALSE);
        break;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

static BOOLEAN PhpInfoCreateWindow(
    _In_ HWND ParentWindowHandle
    )
{
    WNDCLASSEX windowClass;

    if (!InfoClassRegistered)
    {
        memset(&windowClass, 0, sizeof(WNDCLASSEX));
        windowClass.cbSize = sizeof(WNDCLASSEX);
        windowClass.lpfnWndProc = PhpInfoWindowProc;
        windowClass.hInstance = PhInstanceHandle;
        windowClass.hCursor = PhLoadCursor(NULL, IDC_ARROW);
        windowClass.lpszClassName = INFOTAB_CLASS_NAME;

        if (RegisterClassEx(&windowClass) == INVALID_ATOM)
            return FALSE;

        InfoClassRegistered = TRUE;
    }

    InfoHostHandle = PhCreateWindow(
        INFOTAB_CLASS_NAME,
        NULL,
        WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL,
        0, 0, 0, 0,
        ParentWindowHandle,
        NULL,
        PhInstanceHandle,
        NULL
        );

    if (!InfoHostHandle)
        return FALSE;

    InfoFilterHandle = PhCreateWindow(
        WC_EDIT,
        NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        InfoHostHandle,
        UlongToHandle(INFOTAB_ID_FILTER),
        NULL,
        NULL
        );

    if (InfoFilterHandle)
    {
        SendMessage(InfoFilterHandle, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter");
        SendMessage(InfoFilterHandle, EM_SETLIMITTEXT, RTL_NUMBER_OF(InfoFilter) - 1, 0);
    }

    if (PhEnableThemeSupport)
        PhInitializeWindowTheme(InfoHostHandle, PhEnableThemeSupport);

    PhpInfoUpdateFonts(NULL);
    return TRUE;
}

/**
 * Information tab page callback function.
 */
_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
BOOLEAN PhMwpInformationPageCallback(
    _In_ PPH_MAIN_TAB_PAGE Page,
    _In_ PH_MAIN_TAB_PAGE_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case MainTabPageCreate:
        {
            PhMwpInformationPage = Page;
        }
        break;
    case MainTabPageCreateWindow:
        {
            if (!InfoHostHandle && !PhpInfoCreateWindow((HWND)Parameter2))
                return FALSE;

            if (Parameter1)
                *(HWND *)Parameter1 = InfoHostHandle;
        }
        return TRUE;
    case MainTabPageSelected:
        {
            if (!InfoHostHandle)
                break;

            if (PtrToUlong(Parameter1))
            {
                PhpInfoRefreshNow();
                SetTimer(InfoHostHandle, INFOTAB_TIMER_ID, INFOTAB_TIMER_INTERVAL, NULL);
            }
            else
            {
                KillTimer(InfoHostHandle, INFOTAB_TIMER_ID);
            }
        }
        break;
    case MainTabPageFontChanged:
        {
            if (InfoHostHandle)
            {
                PhpInfoUpdateFonts((HFONT)Parameter1);
                InvalidateRect(InfoHostHandle, NULL, FALSE);
            }
        }
        break;
    case MainTabPageDpiChanged:
        {
            if (InfoHostHandle)
            {
                PhpInfoUpdateFonts(InfoFont);
                InvalidateRect(InfoHostHandle, NULL, FALSE);
            }
        }
        break;
    case MainTabPageDestroy:
        {
            if (InfoHostHandle)
                KillTimer(InfoHostHandle, INFOTAB_TIMER_ID);

            if (InfoBoldFont)
            {
                DeleteObject(InfoBoldFont);
                InfoBoldFont = NULL;
            }
        }
        break;
    }

    return FALSE;
}
