/*
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * Authors:
 *
 *     wj32    2010-2015
 *     dmex    2017-2026
 *
 */

#ifndef _PH_PHNET_H
#define _PH_PHNET_H

EXTERN_C_START

#ifndef __WINDOT11_H__
#define __WINDOT11_H__ // Workaround windot11.h C2288 - WinSDK 10.0.22621 (dmex)
#endif

#ifndef PIO_APC_ROUTINE_DEFINED
#define PIO_APC_ROUTINE_DEFINED 1
#endif

#ifndef ALIGN_SIZE
#define ALIGN_SIZE 0x00000008
#endif

#ifndef UM_NDIS60
#define UM_NDIS60 1
#endif

// #include <rtinfo.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2def.h>
#include <windns.h>
#include <nldef.h>
#include <netiodef.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <icmpapi.h>
#include <hvsocket.h>

EXTERN_C CONST DECLSPEC_SELECTANY IN_ADDR  inaddr_any             = { 0x00 };
EXTERN_C CONST DECLSPEC_SELECTANY IN6_ADDR in6addr_any            = { 0x00 };
EXTERN_C CONST DECLSPEC_SELECTANY IN6_ADDR in6addr_v4mappedprefix = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00 };

#define PH_NETWORK_TYPE_NONE 0x0
#define PH_NETWORK_TYPE_IPV4 0x1
#define PH_NETWORK_TYPE_IPV6 0x2
#define PH_NETWORK_TYPE_HYPERV 0x4
#define PH_NETWORK_TYPE_MASK 0x7

#define PH_PROTOCOL_TYPE_NONE 0x0
#define PH_PROTOCOL_TYPE_TCP  0x10
#define PH_PROTOCOL_TYPE_UDP  0x20
#define PH_PROTOCOL_TYPE_MASK 0x30

#define PH_NETWORK_PROTOCOL_NONE 0x0
#define PH_NETWORK_PROTOCOL_TCP4 (PH_NETWORK_TYPE_IPV4 | PH_PROTOCOL_TYPE_TCP)
#define PH_NETWORK_PROTOCOL_TCP6 (PH_NETWORK_TYPE_IPV6 | PH_PROTOCOL_TYPE_TCP)
#define PH_NETWORK_PROTOCOL_UDP4 (PH_NETWORK_TYPE_IPV4 | PH_PROTOCOL_TYPE_UDP)
#define PH_NETWORK_PROTOCOL_UDP6 (PH_NETWORK_TYPE_IPV6 | PH_PROTOCOL_TYPE_UDP)
#define PH_NETWORK_PROTOCOL_HYPERV (PH_NETWORK_TYPE_HYPERV)

typedef struct _PH_IP_ADDRESS
{
    ULONG Type;
    union
    {
        UCHAR Ipv4[4];
        IN_ADDR InAddr;
        UCHAR Ipv6[16];
        IN6_ADDR In6Addr;
        GUID HvAddr;
    };
} PH_IP_ADDRESS, *PPH_IP_ADDRESS;

typedef struct _PH_IP_ENDPOINT
{
    PH_IP_ADDRESS Address;
    ULONG Port;
} PH_IP_ENDPOINT, *PPH_IP_ENDPOINT;

FORCEINLINE
BOOLEAN
PhEqualIpAddress(
    _In_ PPH_IP_ADDRESS Address1,
    _In_ PPH_IP_ADDRESS Address2
    )
{
    if ((Address1->Type | Address2->Type) == 0) // don't check addresses if both are invalid
        return TRUE;
    if (Address1->Type != Address2->Type)
        return FALSE;

    switch (Address1->Type)
    {
    case PH_NETWORK_TYPE_IPV4:
        {
            return IN4_ADDR_EQUAL(&Address1->InAddr, &Address2->InAddr);
        }
        break;
    case PH_NETWORK_TYPE_IPV6:
        {
            return IN6_ADDR_EQUAL(&Address1->In6Addr, &Address2->In6Addr);
//#ifdef _WIN64
//            return
//                *(PULONG64)(Address1->Ipv6) == *(PULONG64)(Address2->Ipv6) &&
//                *(PULONG64)(Address1->Ipv6 + 8) == *(PULONG64)(Address2->Ipv6 + 8);
//#else
//            return
//                *(PULONG)(Address1->Ipv6) == *(PULONG)(Address2->Ipv6) &&
//                *(PULONG)(Address1->Ipv6 + 4) == *(PULONG)(Address2->Ipv6 + 4) &&
//                *(PULONG)(Address1->Ipv6 + 8) == *(PULONG)(Address2->Ipv6 + 8) &&
//                *(PULONG)(Address1->Ipv6 + 12) == *(PULONG)(Address2->Ipv6 + 12);
//#endif
        }
        break;
    case PH_NETWORK_TYPE_HYPERV:
        {
            //return IsEqualGUID(&Address1->HvAddr, &Address2->HvAddr);
            return !memcmp(&Address1->HvAddr, &Address2->HvAddr, sizeof(GUID));
        }
        break;
    }

    return FALSE;
}

FORCEINLINE
ULONG
PhHashIpAddress(
    _In_ PPH_IP_ADDRESS Address
    )
{
    if (Address->Type == PH_NETWORK_TYPE_NONE)
        return 0;

    switch (Address->Type)
    {
    case PH_NETWORK_TYPE_IPV4:
        {
            ULONG hash;

            hash = Address->Type | (Address->Type << 16);
            hash ^= *(PULONG)(Address->Ipv4);

            return hash;
        }
        break;
    case PH_NETWORK_TYPE_IPV6:
        {
            ULONG hash;

            hash = Address->Type | (Address->Type << 16);
            hash += *(PULONG)(Address->Ipv6);
            hash ^= *(PULONG)(Address->Ipv6 + 4);
            hash += *(PULONG)(Address->Ipv6 + 8);
            hash ^= *(PULONG)(Address->Ipv6 + 12);

            return hash;
        }
        break;
    case PH_NETWORK_TYPE_HYPERV:
        {
            ULONG hash;

            hash = Address->Type | (Address->Type << 16);
            hash += Address->HvAddr.Data1;
            hash ^= *(PULONG)(&Address->HvAddr.Data2);
            hash ^= *(PULONG)(Address->HvAddr.Data4);
            hash ^= *(PULONG)(Address->HvAddr.Data4 + 4);

            return hash;
        }
        break;
    }

    return 0;
}

FORCEINLINE
BOOLEAN
PhIsNullIpAddress(
    _In_ PPH_IP_ADDRESS Address
    )
{
    if (Address->Type == PH_NETWORK_TYPE_NONE)
        return TRUE;

    switch (Address->Type)
    {
    case PH_NETWORK_TYPE_IPV4:
        {
            return IN4_IS_ADDR_UNSPECIFIED(&Address->InAddr);
        }
        break;
    case PH_NETWORK_TYPE_IPV6:
        {
            return IN6_IS_ADDR_UNSPECIFIED(&Address->In6Addr);
//#ifdef _WIN64
//            return (*(PULONG64)(Address->Ipv6) | *(PULONG64)(Address->Ipv6 + 8)) == 0;
//#else
//            return (*(PULONG)(Address->Ipv6) | *(PULONG)(Address->Ipv6 + 4) |
//                *(PULONG)(Address->Ipv6 + 8) | *(PULONG)(Address->Ipv6 + 12)) == 0;
//#endif
        }
        break;
    case PH_NETWORK_TYPE_HYPERV:
        {
            //return IsEqualGUID(&Address->HvAddr, &HV_GUID_ZERO);
            return !memcmp(&Address->HvAddr, &HV_GUID_ZERO, sizeof(GUID));
        }
        break;
    }

    return TRUE;
}

FORCEINLINE
BOOLEAN
PhEqualIpEndpoint(
    _In_ PPH_IP_ENDPOINT Endpoint1,
    _In_ PPH_IP_ENDPOINT Endpoint2
    )
{
    return PhEqualIpAddress(&Endpoint1->Address, &Endpoint2->Address) &&
        Endpoint1->Port == Endpoint2->Port;
}

FORCEINLINE
ULONG
PhHashIpEndpoint(
    _In_ PPH_IP_ENDPOINT Endpoint
    )
{
    return PhHashIpAddress(&Endpoint->Address) ^ Endpoint->Port;
}

//
// IPv4 and IPv6
//

PHLIBAPI
NTSTATUS
NTAPI
PhIpv4AddressToStringEx(
    _In_ PCIN_ADDR Address,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv4StringToAddressEx(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Strict,
    _Out_ PIN_ADDR Address,
    _Out_ PUSHORT Port
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv6AddressToStringEx(
    _In_ PCIN6_ADDR Address,
    _In_ ULONG ScopeId,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv6StringToAddressEx(
    _In_ PCWSTR AddressString,
    _Out_ PIN6_ADDR Address,
    _Out_ PULONG ScopeId,
    _Out_ PUSHORT Port
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv4AddressToString(
    _In_ PCIN_ADDR Address,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv4StringToAddress(
    _In_ PCWSTR AddressString,
    _In_ BOOLEAN Strict,
    _Out_ PIN_ADDR Address,
    _Out_ PUSHORT Port
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv6AddressToString(
    _In_ PCIN6_ADDR Address,
    _In_ ULONG ScopeId,
    _In_ USHORT Port,
    _Out_writes_to_(*AddressStringLength, *AddressStringLength) PWSTR AddressString,
    _Inout_ PULONG AddressStringLength
    );

PHLIBAPI
NTSTATUS
NTAPI
PhIpv6StringToAddress(
    _In_ PCWSTR AddressString,
    _Out_ PIN6_ADDR Address,
    _Out_ PULONG ScopeId,
    _Out_ PUSHORT Port
    );

EXTERN_C_END

#endif
