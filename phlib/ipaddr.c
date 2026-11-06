/*
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * Authors:
 *
 *     dmex    2017-2026
 *
 */

#include <ph.h>
#include <phnet.h>

//
// IPv4 and IPv6
//

static PWSTR PhIpAddressWriteUInt32(
    _Out_ PWSTR Buffer,
    _In_ ULONG Value,
    _In_ ULONG Base
    )
{
    WCHAR digits[16];
    ULONG count = 0;

    do
    {
        digits[count++] = L"0123456789abcdef"[Value % Base];
        Value /= Base;
    } while (Value);

    while (count)
    {
        *Buffer++ = digits[--count];
    }

    return Buffer;
}

static BOOLEAN PhpParseIpComponent(
    _Inout_ PCWSTR* AddressString,
    _In_ ULONG Base,
    _Out_ PULONG Value
    )
{
    PCWSTR string = *AddressString;
    ULONG value = 0;
    ULONG digit;
    BOOLEAN found = FALSE;

    while (*string)
    {
        if (*string >= L'0' && *string <= L'9')
            digit = *string - L'0';
        else if (*string >= L'a' && *string <= L'f')
            digit = *string - L'a' + 10;
        else if (*string >= L'A' && *string <= L'F')
            digit = *string - L'A' + 10;
        else
            break;

        if (digit >= Base || value > (MAXULONG - digit) / Base)
            return FALSE;

        value = value * Base + digit;
        found = TRUE;
        string++;
    }

    if (!found)
        return FALSE;

    *AddressString = string;
    *Value = value;
    return TRUE;
}

static NTSTATUS PhpIpv4StringToAddress(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Strict,
    _Out_opt_ PCWSTR* Terminator,
    _Out_ PIN_ADDR Address,
    _Out_opt_ PUSHORT Port
    )
{
    PCWSTR string = AddressString;
    ULONG fields[4];
    ULONG count = 0;
    ULONG base;
    ULONG value;
    IN_ADDR address;

    if (!AddressString || !Address)
        return STATUS_INVALID_PARAMETER;

    for (;;)
    {
        base = 10;

        if (*string == L'0')
        {
            if (string[1] == L'x' || string[1] == L'X')
            {
                if (Strict)
                    goto InvalidAddress;

                base = 16;
                string += 2;
            }
            else if (string[1] >= L'0' && string[1] <= L'9')
            {
                if (Strict)
                    goto InvalidAddress;

                base = 8;
                string++;
            }
        }

        if (!PhpParseIpComponent(&string, base, &fields[count]))
            goto InvalidAddress;

        count++;

        if (*string != L'.')
            break;
        if (count == RTL_NUMBER_OF(fields))
            goto InvalidAddress;

        string++;
    }

    if (Strict && count != 4)
        goto InvalidAddress;

    switch (count)
    {
    case 1:
        value = fields[0];
        address.S_un.S_un_b.s_b1 = (UCHAR)(value >> 24);
        address.S_un.S_un_b.s_b2 = (UCHAR)(value >> 16);
        address.S_un.S_un_b.s_b3 = (UCHAR)(value >> 8);
        address.S_un.S_un_b.s_b4 = (UCHAR)value;
        break;
    case 2:
        if (fields[0] > MAXUCHAR || fields[1] > 0xffffff)
            goto InvalidAddress;
        address.S_un.S_un_b.s_b1 = (UCHAR)fields[0];
        address.S_un.S_un_b.s_b2 = (UCHAR)(fields[1] >> 16);
        address.S_un.S_un_b.s_b3 = (UCHAR)(fields[1] >> 8);
        address.S_un.S_un_b.s_b4 = (UCHAR)fields[1];
        break;
    case 3:
        if (fields[0] > MAXUCHAR || fields[1] > MAXUCHAR || fields[2] > MAXUSHORT)
            goto InvalidAddress;
        address.S_un.S_un_b.s_b1 = (UCHAR)fields[0];
        address.S_un.S_un_b.s_b2 = (UCHAR)fields[1];
        address.S_un.S_un_b.s_b3 = (UCHAR)(fields[2] >> 8);
        address.S_un.S_un_b.s_b4 = (UCHAR)fields[2];
        break;
    case 4:
        if (fields[0] > MAXUCHAR || fields[1] > MAXUCHAR || fields[2] > MAXUCHAR || fields[3] > MAXUCHAR)
            goto InvalidAddress;
        address.S_un.S_un_b.s_b1 = (UCHAR)fields[0];
        address.S_un.S_un_b.s_b2 = (UCHAR)fields[1];
        address.S_un.S_un_b.s_b3 = (UCHAR)fields[2];
        address.S_un.S_un_b.s_b4 = (UCHAR)fields[3];
        break;
    default:
        goto InvalidAddress;
    }

    if (Terminator)
        *Terminator = string;

    if (Port)
    {
        *Port = 0;

        if (*string == L':')
        {
            string++;

            if (!PhpParseIpComponent(&string, 10, &value) || value == 0 || value > MAXUSHORT || *string)
                goto InvalidAddress;

            *Port = RtlUshortByteSwap((USHORT)value);
        }
        else if (*string)
        {
            goto InvalidAddress;
        }
    }

    *Address = address;
    return STATUS_SUCCESS;

InvalidAddress:
    if (Terminator)
        *Terminator = string;
    return STATUS_INVALID_PARAMETER;
}

static PWSTR PhpIpv4AddressToString(
    _In_ PCIN_ADDR Address,
    _Out_writes_(16) PWSTR AddressString
    )
{
    PWSTR string = AddressString;

    string = PhIpAddressWriteUInt32(string, Address->S_un.S_un_b.s_b1, 10); *string++ = L'.';
    string = PhIpAddressWriteUInt32(string, Address->S_un.S_un_b.s_b2, 10); *string++ = L'.';
    string = PhIpAddressWriteUInt32(string, Address->S_un.S_un_b.s_b3, 10); *string++ = L'.';
    string = PhIpAddressWriteUInt32(string, Address->S_un.S_un_b.s_b4, 10); *string = UNICODE_NULL;

    return string;
}

NTSTATUS PhIpv4AddressToStringEx(
    _In_ PCIN_ADDR Address,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    WCHAR buffer[22];
    PWSTR string;
    ULONG length;

    if (!Address || !AddressString || !AddressStringLength)
        return STATUS_INVALID_PARAMETER;

    string = PhpIpv4AddressToString(Address, buffer);

    if (Port)
    {
        *string++ = L':';
        string = PhIpAddressWriteUInt32(string, RtlUshortByteSwap(Port), 10);
        *string = UNICODE_NULL;
    }

    length = (ULONG)(string - buffer) + 1;

    if (*AddressStringLength < length)
    {
        *AddressStringLength = length;
        return STATUS_INVALID_PARAMETER;
    }

    memcpy(AddressString, buffer, length * sizeof(WCHAR));
    *AddressStringLength = length;
    return STATUS_SUCCESS;
#else
    return RtlIpv4AddressToStringEx(Address, Port, AddressString, AddressStringLength);
#endif
}

static PWSTR PhpIpv6AddressToString(
    _In_ PCIN6_ADDR Address,
    _Out_writes_(46) PWSTR AddressString
    )
{
    USHORT words[8];
    ULONG index;
    ULONG bestStart = 8;
    ULONG bestLength = 0;
    ULONG currentStart;
    ULONG currentLength;
    ULONG ipv6End = 8;
    BOOLEAN embeddedIpv4;
    PWSTR string = AddressString;

    for (index = 0; index < RTL_NUMBER_OF(words); index++)
        words[index] = RtlUshortByteSwap(Address->u.Word[index]);

    embeddedIpv4 =
        (words[5] == 0x5efe && !(words[4] & ~0x200)) ||
        (!words[0] && !words[1] && !words[2] && !words[3] &&
            (!words[4] || words[4] == 0xffff) &&
            ((!words[4] && (!words[5] || words[5] == 0xffff)) || (words[4] == 0xffff && !words[5])) &&
            words[6]);

    if (embeddedIpv4)
        ipv6End = 6;

    for (index = 0; index < ipv6End;)
    {
        if (words[index])
        {
            index++;
            continue;
        }

        currentStart = index;
        while (index < ipv6End && !words[index])
            index++;
        currentLength = index - currentStart;

        if (currentLength > bestLength)
        {
            bestStart = currentStart;
            bestLength = currentLength;
        }
    }

    if (bestLength < 2)
        bestStart = ipv6End;

    for (index = 0; index < ipv6End; index++)
    {
        if (index == bestStart)
        {
            *string++ = L':';
            *string++ = L':';
            index += bestLength - 1;
            continue;
        }

        if (index && index != bestStart + bestLength)
            *string++ = L':';

        string = PhIpAddressWriteUInt32(string, words[index], 16);
    }

    if (embeddedIpv4)
    {
        if (string != AddressString && string[-1] != L':')
            *string++ = L':';
        string = PhpIpv4AddressToString((PCIN_ADDR)&Address->u.Byte[12], string);
    }

    *string = UNICODE_NULL;
    return string;
}

NTSTATUS PhIpv6AddressToStringEx(
    _In_ PCIN6_ADDR Address,
    _In_ ULONG ScopeId,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    WCHAR buffer[65];
    PWSTR string = buffer;
    ULONG length;

    if (!Address || !AddressString || !AddressStringLength)
        return STATUS_INVALID_PARAMETER;

    if (Port)
        *string++ = L'[';

    string = PhpIpv6AddressToString(Address, string);

    if (ScopeId)
    {
        *string++ = L'%';
        string = PhIpAddressWriteUInt32(string, ScopeId, 10);
    }

    if (Port)
    {
        *string++ = L']';
        *string++ = L':';
        string = PhIpAddressWriteUInt32(string, RtlUshortByteSwap(Port), 10);
    }

    *string = UNICODE_NULL;
    length = (ULONG)(string - buffer) + 1;

    if (*AddressStringLength < length)
    {
        *AddressStringLength = length;
        return STATUS_INVALID_PARAMETER;
    }

    memcpy(AddressString, buffer, length * sizeof(WCHAR));
    *AddressStringLength = length;
    return STATUS_SUCCESS;
#else
    return RtlIpv6AddressToStringEx(Address, ScopeId, Port, AddressString, AddressStringLength);
#endif
}

static NTSTATUS PhpIpv4StringToAddressBasic(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Strict,
    _Out_ PCWSTR* Terminator,
    _Out_ PIN_ADDR Address
    )
{
    return PhpIpv4StringToAddress(AddressString, Strict, Terminator, Address, NULL);
}

NTSTATUS PhIpv4StringToAddressEx(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Strict,
    _Out_ PIN_ADDR Address,
    _Out_ PUSHORT Port
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    if (!Port)
        return STATUS_INVALID_PARAMETER;

    return PhpIpv4StringToAddress(AddressString, Strict, NULL, Address, Port);
#else
    return RtlIpv4StringToAddressEx(AddressString, Strict, Address, Port);
#endif
}

static NTSTATUS PhpIpv6StringToAddress(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Extended,
    _Out_opt_ PCWSTR* Terminator,
    _Out_ PIN6_ADDR Address,
    _Out_opt_ PULONG ScopeId,
    _Out_opt_ PUSHORT Port
    )
{
    PCWSTR string = AddressString;
    IN6_ADDR address = { 0 };
    UCHAR bytes[16];
    ULONG byteCount = 0;
    LONG gap = -1;
    ULONG value;
    ULONG digits;
    PCWSTR component;
    ULONG scope = 0;
    USHORT port = 0;
    BOOLEAN bracketed = FALSE;

    if (!AddressString || !Address)
        return STATUS_INVALID_PARAMETER;

    if (*string == L'[')
    {
        if (!Extended)
            goto InvalidAddress;
        bracketed = TRUE;
        string++;
    }

    if (*string == L':')
    {
        if (string[1] != L':')
            goto InvalidAddress;
        gap = 0;
        string += 2;
    }

    while (*string && *string != L'%' && *string != L']')
    {
        component = string;
        digits = 0;
        value = 0;

        while (((*string >= L'0' && *string <= L'9') || (*string >= L'a' && *string <= L'f') || (*string >= L'A' && *string <= L'F')) && digits < 8)
        {
            ULONG digit = *string <= L'9' ? *string - L'0' : ((*string | 0x20) - L'a' + 10);
            value = value * 16 + digit;
            string++;
            digits++;
        }

        if (*string == L'.')
        {
            IN_ADDR address4;
            PCWSTR terminator4;

            if (byteCount > 12 || !NT_SUCCESS(PhpIpv4StringToAddress(component, TRUE, &terminator4, &address4, NULL)))
                goto InvalidAddress;

            memcpy(bytes + byteCount, &address4, sizeof(address4));
            byteCount += sizeof(address4);
            string = terminator4;
            break;
        }

        if (!digits || digits > 4 || value > MAXUSHORT || byteCount > 14)
            goto InvalidAddress;

        bytes[byteCount++] = (UCHAR)(value >> 8);
        bytes[byteCount++] = (UCHAR)value;

        if (*string != L':')
            break;

        string++;
        if (*string == L':')
        {
            if (gap != -1)
                goto InvalidAddress;
            gap = (LONG)byteCount;
            string++;
            if (!*string || *string == L'%' || *string == L']')
                break;
        }
    }

    if (gap >= 0)
    {
        ULONG tailLength;

        if (byteCount >= sizeof(bytes))
            goto InvalidAddress;

        tailLength = byteCount - gap;
        memmove(bytes + sizeof(bytes) - tailLength, bytes + gap, tailLength);
        memset(bytes + gap, 0, sizeof(bytes) - byteCount);
        byteCount = sizeof(bytes);
    }

    if (byteCount != sizeof(bytes))
        goto InvalidAddress;

    if (Extended && *string == L'%')
    {
        string++;
        if (!PhpParseIpComponent(&string, 10, &scope))
            goto InvalidAddress;
    }

    if (bracketed)
    {
        if (*string++ != L']')
            goto InvalidAddress;

        if (*string == L':')
        {
            string++;
            if (!PhpParseIpComponent(&string, 10, &value) || value > MAXUSHORT)
                goto InvalidAddress;
            port = RtlUshortByteSwap((USHORT)value);
        }
    }

    if (Extended && *string)
        goto InvalidAddress;

    memcpy(&address, bytes, sizeof(address));
    *Address = address;
    if (ScopeId) *ScopeId = scope;
    if (Port) *Port = port;
    if (Terminator) *Terminator = string;
    return STATUS_SUCCESS;

InvalidAddress:
    if (Terminator) *Terminator = string;
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS PhpIpv6StringToAddressBasic(
    _In_ PCWSTR AddressString,
    _Out_ PCWSTR* Terminator,
    _Out_ PIN6_ADDR Address
    )
{
    return PhpIpv6StringToAddress(AddressString, FALSE, Terminator, Address, NULL, NULL);
}

NTSTATUS PhIpv6StringToAddressEx(
    _In_ PCWSTR AddressString,
    _Out_ PIN6_ADDR Address,
    _Out_ PULONG ScopeId,
    _Out_ PUSHORT Port
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    if (!ScopeId || !Port)
        return STATUS_INVALID_PARAMETER;

    return PhpIpv6StringToAddress(AddressString, TRUE, NULL, Address, ScopeId, Port);
#else
    return RtlIpv6StringToAddressEx(AddressString, Address, ScopeId, Port);
#endif
}

NTSTATUS PhIpv4AddressToString(
    _In_ PCIN_ADDR Address,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    return PhIpv4AddressToStringEx(Address, Port, AddressString, AddressStringLength);
#else
    return RtlIpv4AddressToStringEx(Address, Port, AddressString, AddressStringLength);
#endif
}

NTSTATUS PhIpv4StringToAddress(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Strict,
    _Out_ PIN_ADDR Address,
    _Out_ PUSHORT Port
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    return PhIpv4StringToAddressEx(AddressString, Strict, Address, Port);
#else
    return RtlIpv4StringToAddressEx(AddressString, Strict, Address, Port);
#endif
}

NTSTATUS PhIpv6AddressToString(
    _In_ PCIN6_ADDR Address,
    _In_ ULONG ScopeId,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    return PhIpv6AddressToStringEx(Address, ScopeId, Port, AddressString, AddressStringLength);
#else
    return RtlIpv6AddressToStringEx(Address, ScopeId, Port, AddressString, AddressStringLength);
#endif
}

NTSTATUS PhIpv6StringToAddress(
    _In_ PCWSTR AddressString,
    _Out_ PIN6_ADDR Address,
    _Out_ PULONG ScopeId,
    _Out_ PUSHORT Port
    )
{
#ifndef PH_NATIVE_IPADDRESS_STRING
    return PhIpv6StringToAddressEx(AddressString, Address, ScopeId, Port);
#else
    return RtlIpv6StringToAddressEx(AddressString, Address, ScopeId, Port);
#endif
}
