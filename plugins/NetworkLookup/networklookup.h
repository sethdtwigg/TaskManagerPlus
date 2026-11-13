/*
 * TaskManagerPlus NetworkLookup plugin.
 *
 * Offline lookups for the Network tab: port -> service name (IANA registry, bundled) and remote
 * address -> country (RIR delegation statistics, bundled). No network access of any kind.
 */

#ifndef _NETWORKLOOKUP_H_
#define _NETWORKLOOKUP_H_

#include <phdk.h>
#include <phappresource.h>
#include <settings.h>
#include "resource.h"

#define PLUGIN_NAME L"TaskManagerPlus.NetworkLookup"

#define NLC_LOCAL_SERVICE 1
#define NLC_REMOTE_SERVICE 2
#define NLC_REMOTE_COUNTRY 3

extern PPH_PLUGIN PluginInstance;

// ports.c

/** Returns the static service name for a port/protocol (PH_PROTOCOL_TYPE_*), or NULL. Description may be empty. */
PCWSTR NlLookupService(
    _In_ ULONG Port,
    _In_ ULONG ProtocolType,
    _Out_opt_ PCWSTR *Description
    );

// country.c

typedef enum _NL_ADDRESS_CLASS
{
    NlAddressNone, // no address (listening sockets, ::, 0.0.0.0)
    NlAddressPublic,
    NlAddressLocal, // loopback, private, link-local, unspecified
    NlAddressMulticast
} NL_ADDRESS_CLASS;

BOOLEAN NlCountryInitialize(
    VOID
    );

/**
 * Looks up an address. Returns the country name (static) or NULL if unknown; Code receives the two letter code
 * (static, may be NULL) and Class how the address was classified. Public addresses without an entry return NULL.
 */
PCWSTR NlLookupCountry(
    _In_ PPH_IP_ADDRESS Address,
    _Out_opt_ PCWSTR *Code,
    _Out_ NL_ADDRESS_CLASS *Class
    );

ULONG NlGetCountryTableDate(
    VOID
    );

#endif
