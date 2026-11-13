/*
 * TaskManagerPlus NetworkLookup plugin: address -> country from the bundled RIR delegation table.
 *
 * country.bin is embedded as an RCDATA resource (layout in tools/make-lookup-tables.py). The lookup is a
 * binary search; there is no network access and no GeoIP database. Accuracy is registry-level: the country
 * is where the address block was delegated, not where a host physically is.
 */

#include "networklookup.h"
#include "country_info.h"

typedef struct _NL_COUNTRY_NAME
{
    USHORT Packed;
    PCWSTR Code;
    PCWSTR Name;
} NL_COUNTRY_NAME;

// Generated, sorted by code.
static CONST NL_COUNTRY_NAME NlCountryNames[] =
{
#include "countries_data.h"
};

#include <pshpack1.h>
typedef struct _NL_RANGE
{
    ULONG Start;
    USHORT Country;
} NL_RANGE;
#include <poppack.h>

static CONST NL_RANGE *NlV4Table = NULL;
static ULONG NlV4Count = 0;
static CONST NL_RANGE *NlV6Table = NULL;
static ULONG NlV6Count = 0;
static ULONG NlTableDate = 0;

BOOLEAN NlCountryInitialize(
    VOID
    )
{
    HRSRC resource;
    HGLOBAL global;
    ULONG size;
    PUCHAR data;
    ULONG v4;
    ULONG v6;

    resource = FindResourceW(PluginInstance->DllBase, MAKEINTRESOURCEW(IDR_COUNTRY_TABLE), RT_RCDATA);

    if (!resource)
        return FALSE;

    size = SizeofResource(PluginInstance->DllBase, resource);
    global = LoadResource(PluginInstance->DllBase, resource);

    if (!global || size < 16)
        return FALSE;

    data = LockResource(global);

    if (memcmp(data, "TCT1", 4) != 0)
        return FALSE;

    memcpy(&v4, data + 4, sizeof(ULONG));
    memcpy(&v6, data + 8, sizeof(ULONG));
    memcpy(&NlTableDate, data + 12, sizeof(ULONG));

    if ((ULONGLONG)16 + ((ULONGLONG)v4 + v6) * sizeof(NL_RANGE) > size)
        return FALSE;

    NlV4Table = (CONST NL_RANGE *)(data + 16);
    NlV4Count = v4;
    NlV6Table = NlV4Table + v4;
    NlV6Count = v6;

    return TRUE;
}

ULONG NlGetCountryTableDate(
    VOID
    )
{
    return NlTableDate;
}

static USHORT NlFind(
    _In_ CONST NL_RANGE *Table,
    _In_ ULONG Count,
    _In_ ULONG Key
    )
{
    ULONG low = 0;
    ULONG high = Count;
    NL_RANGE entry;

    // last entry with Start <= Key
    while (low < high)
    {
        ULONG middle = (low + high) / 2;

        memcpy(&entry, &Table[middle], sizeof(NL_RANGE));

        if (entry.Start <= Key)
            low = middle + 1;
        else
            high = middle;
    }

    if (low == 0)
        return 0;

    memcpy(&entry, &Table[low - 1], sizeof(NL_RANGE));

    return entry.Country;
}

static PCWSTR NlNameFromPacked(
    _In_ USHORT Packed,
    _Out_opt_ PCWSTR *Code
    )
{
    ULONG low = 0;
    ULONG high = RTL_NUMBER_OF(NlCountryNames);

    while (low < high)
    {
        ULONG middle = (low + high) / 2;

        if (NlCountryNames[middle].Packed < Packed)
            low = middle + 1;
        else
            high = middle;
    }

    if (low < RTL_NUMBER_OF(NlCountryNames) && NlCountryNames[low].Packed == Packed)
    {
        if (Code)
            *Code = NlCountryNames[low].Code;
        return NlCountryNames[low].Name;
    }

    return NULL;
}

static NL_ADDRESS_CLASS NlClassifyV4(
    _In_ ULONG Ip // host order
    )
{
    ULONG a = Ip >> 24;
    ULONG b = (Ip >> 16) & 0xFF;

    if (a == 0)
        return NlAddressNone;
    if (a == 10 || a == 127)
        return NlAddressLocal;
    if (a == 169 && b == 254)
        return NlAddressLocal;
    if (a == 172 && b >= 16 && b <= 31)
        return NlAddressLocal;
    if (a == 192 && b == 168)
        return NlAddressLocal;
    if (a == 100 && b >= 64 && b <= 127) // carrier-grade NAT
        return NlAddressLocal;
    if (a >= 224 && a <= 239)
        return NlAddressMulticast;
    if (Ip == 0xFFFFFFFF)
        return NlAddressLocal;

    return NlAddressPublic;
}

PCWSTR NlLookupCountry(
    _In_ PPH_IP_ADDRESS Address,
    _Out_opt_ PCWSTR *Code,
    _Out_ NL_ADDRESS_CLASS *Class
    )
{
    USHORT packed;

    if (Code)
        *Code = NULL;

    *Class = NlAddressNone;

    if (Address->Type == PH_NETWORK_TYPE_IPV4)
    {
        ULONG ip = ((ULONG)Address->Ipv4[0] << 24) | ((ULONG)Address->Ipv4[1] << 16) | ((ULONG)Address->Ipv4[2] << 8) | Address->Ipv4[3];

        *Class = NlClassifyV4(ip);

        if (*Class != NlAddressPublic || !NlV4Table)
            return NULL;

        packed = NlFind(NlV4Table, NlV4Count, ip);
    }
    else if (Address->Type == PH_NETWORK_TYPE_IPV6)
    {
        PUCHAR b = Address->Ipv6;
        ULONG top = ((ULONG)b[0] << 24) | ((ULONG)b[1] << 16) | ((ULONG)b[2] << 8) | b[3];
        BOOLEAN zeroPrefix = TRUE;

        for (ULONG i = 0; i < 15; i++)
        {
            if (b[i])
                zeroPrefix = FALSE;
        }

        if (zeroPrefix && b[15] == 0)
        {
            *Class = NlAddressNone; // ::
            return NULL;
        }

        if (zeroPrefix && b[15] == 1)
        {
            *Class = NlAddressLocal; // ::1
            return NULL;
        }

        // IPv4-mapped ::ffff:a.b.c.d
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
            b[8] == 0 && b[9] == 0 && b[10] == 0xFF && b[11] == 0xFF)
        {
            PH_IP_ADDRESS v4;

            memset(&v4, 0, sizeof(v4));
            v4.Type = PH_NETWORK_TYPE_IPV4;
            memcpy(v4.Ipv4, b + 12, 4);
            return NlLookupCountry(&v4, Code, Class);
        }

        if (b[0] == 0xFF)
        {
            *Class = NlAddressMulticast;
            return NULL;
        }

        if ((b[0] == 0xFE && (b[1] & 0xC0) == 0x80) || (b[0] & 0xFE) == 0xFC)
        {
            *Class = NlAddressLocal; // link-local, unique local
            return NULL;
        }

        *Class = NlAddressPublic;

        if (!NlV6Table)
            return NULL;

        packed = NlFind(NlV6Table, NlV6Count, top);
    }
    else
    {
        return NULL;
    }

    if (!packed)
        return NULL;

    return NlNameFromPacked(packed, Code);
}
