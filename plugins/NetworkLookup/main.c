/*
 * TaskManagerPlus NetworkLookup plugin: registration, Network tab columns and tooltips.
 *
 * Adds "Local service", "Remote service" (IANA port registry) and "Remote country" (RIR delegation
 * statistics) columns to the Network tab. Both tables are bundled in the plugin; nothing is downloaded.
 */

#include "networklookup.h"

PPH_PLUGIN PluginInstance = NULL;

static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginTreeNewMessageCallbackRegistration;
static PH_CALLBACK_REGISTRATION NetworkTreeNewInitializingCallbackRegistration;
static HWND NetworkTreeNewHandle = NULL;

/** Text of one cell as a static string (never freed). */
static PCWSTR NlGetCellString(
    _In_ PPH_NETWORK_ITEM NetworkItem,
    _In_ ULONG SubId
    )
{
    switch (SubId)
    {
    case NLC_LOCAL_SERVICE:
        {
            PCWSTR name = NlLookupService(NetworkItem->LocalEndpoint.Port, NetworkItem->ProtocolType, NULL);
            return name ? name : L"";
        }
    case NLC_REMOTE_SERVICE:
        {
            PCWSTR name = NlLookupService(NetworkItem->RemoteEndpoint.Port, NetworkItem->ProtocolType, NULL);
            return name ? name : L"";
        }
    case NLC_REMOTE_COUNTRY:
        {
            NL_ADDRESS_CLASS addressClass;
            PCWSTR name = NlLookupCountry(&NetworkItem->RemoteEndpoint.Address, NULL, &addressClass);

            if (name)
                return name;

            switch (addressClass)
            {
            case NlAddressLocal:
                return L"Local network";
            case NlAddressMulticast:
                return L"Multicast";
            case NlAddressPublic:
                return L"Unknown";
            }
        }
    }

    return L"";
}

static VOID NlFormatTooltip(
    _In_ PPH_NETWORK_ITEM NetworkItem,
    _In_ ULONG SubId,
    _Out_writes_(Count) PWSTR Buffer,
    _In_ ULONG Count
    )
{
    PCWSTR transport = (NetworkItem->ProtocolType & PH_PROTOCOL_TYPE_TCP) ? L"TCP" : L"UDP";

    Buffer[0] = UNICODE_NULL;

    switch (SubId)
    {
    case NLC_LOCAL_SERVICE:
    case NLC_REMOTE_SERVICE:
        {
            ULONG port = SubId == NLC_LOCAL_SERVICE ? NetworkItem->LocalEndpoint.Port : NetworkItem->RemoteEndpoint.Port;
            PCWSTR description;
            PCWSTR name = NlLookupService(port, NetworkItem->ProtocolType, &description);

            if (!name)
            {
                if (port)
                    swprintf_s(Buffer, Count, L"%s port %lu: no service name in the IANA registry", transport, port);
                break;
            }

            if (description && description[0])
                swprintf_s(Buffer, Count, L"%s port %lu: %s\n%s\n(IANA registry, bundled offline)", transport, port, name, description);
            else
                swprintf_s(Buffer, Count, L"%s port %lu: %s\n(IANA registry, bundled offline)", transport, port, name);
        }
        break;
    case NLC_REMOTE_COUNTRY:
        {
            NL_ADDRESS_CLASS addressClass;
            PCWSTR code;
            PCWSTR name = NlLookupCountry(&NetworkItem->RemoteEndpoint.Address, &code, &addressClass);
            ULONG date = NlGetCountryTableDate();

            if (name)
            {
                swprintf_s(
                    Buffer, Count,
                    L"%s (%s)\nWhere the address block was delegated (RIR statistics of %lu-%02lu-%02lu, bundled offline); not the host's physical location.",
                    name, code ? code : L"", date / 10000, (date / 100) % 100, date % 100
                    );
            }
            else if (addressClass == NlAddressLocal)
            {
                wcscpy_s(Buffer, Count, L"Loopback, private, link-local or shared address space: this machine or a local network.");
            }
            else if (addressClass == NlAddressMulticast)
            {
                wcscpy_s(Buffer, Count, L"Multicast address.");
            }
            else if (addressClass == NlAddressPublic)
            {
                wcscpy_s(Buffer, Count, L"No country in the bundled table (unallocated or newer than the table).");
            }
        }
        break;
    }
}

_Function_class_(PH_PLUGIN_TREENEW_SORT_FUNCTION)
static LONG NTAPI NlNetworkSortFunction(
    _In_ PVOID Node1,
    _In_ PVOID Node2,
    _In_ ULONG SubId,
    _In_ PH_SORT_ORDER SortOrder,
    _In_ PVOID Context
    )
{
    PPH_NETWORK_NODE node1 = Node1;
    PPH_NETWORK_NODE node2 = Node2;
    LONG result = _wcsicmp(NlGetCellString(node1->NetworkItem, SubId), NlGetCellString(node2->NetworkItem, SubId));

    return PhModifySort(result, SortOrder);
}

static VOID NlAddColumn(
    _In_ PPH_PLUGIN_TREENEW_INFORMATION TreeNewInfo,
    _In_ ULONG SubId,
    _In_ PWSTR Text,
    _In_ ULONG Width,
    _In_ BOOLEAN Visible,
    _In_ ULONG DisplayIndex
    )
{
    PH_TREENEW_COLUMN column;

    memset(&column, 0, sizeof(PH_TREENEW_COLUMN));
    column.Text = Text;
    column.Width = Width;
    column.Alignment = PH_ALIGN_LEFT;
    column.Visible = Visible; // a saved column set overrides this
    column.DisplayIndex = DisplayIndex; // after the Remote port column

    PhPluginAddTreeNewColumn(PluginInstance, TreeNewInfo->CmData, &column, SubId, NULL, NlNetworkSortFunction);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    NlCountryInitialize();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI NetworkTreeNewInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_INFORMATION treeNewInfo = Parameter;

    NetworkTreeNewHandle = treeNewInfo->TreeNewHandle;

    NlAddColumn(treeNewInfo, NLC_LOCAL_SERVICE, L"Local service", 90, FALSE, 0);
    NlAddColumn(treeNewInfo, NLC_REMOTE_SERVICE, L"Remote service", 90, TRUE, 6);
    NlAddColumn(treeNewInfo, NLC_REMOTE_COUNTRY, L"Remote country", 110, TRUE, 7);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI TreeNewMessageCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_TREENEW_MESSAGE message = Parameter;
    PPH_NETWORK_NODE node;
    static WCHAR tooltip[512]; // main thread only; the tree copies the text before the callback returns to its caller

    if (!message || message->TreeNewHandle != NetworkTreeNewHandle)
        return;

    switch (message->Message)
    {
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = message->Parameter1;

            node = (PPH_NETWORK_NODE)getCellText->Node;
            PhInitializeStringRefLongHint(&getCellText->Text, NlGetCellString(node->NetworkItem, message->SubId));
        }
        break;
    case TreeNewGetCellTooltip:
        {
            PPH_TREENEW_GET_CELL_TOOLTIP getCellTooltip = message->Parameter1;

            node = (PPH_NETWORK_NODE)getCellTooltip->Node;
            NlFormatTooltip(node->NetworkItem, message->SubId, tooltip, RTL_NUMBER_OF(tooltip));

            if (tooltip[0])
            {
                PhInitializeStringRefLongHint(&getCellTooltip->Text, tooltip);
                getCellTooltip->Unfolding = FALSE;
                getCellTooltip->MaximumWidth = 420;
            }
        }
        break;
    }
}

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        PPH_PLUGIN_INFORMATION info;

        PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

        if (!PluginInstance)
            return FALSE;

        info->DisplayName = L"Network Lookup";
        info->Author = L"TaskManagerPlus";
        info->Description = L"Adds service names for ports (IANA registry) and a country column for remote addresses"
            L" (RIR delegation statistics) to the Network tab, from tables bundled in the plugin. No network access.";
        info->HasOptions = FALSE;

        PhRegisterCallback(
            PhGetPluginCallback(PluginInstance, PluginCallbackLoad),
            LoadCallback,
            NULL,
            &PluginLoadCallbackRegistration
            );
        PhRegisterCallback(
            PhGetPluginCallback(PluginInstance, PluginCallbackTreeNewMessage),
            TreeNewMessageCallback,
            NULL,
            &PluginTreeNewMessageCallbackRegistration
            );
        PhRegisterCallback(
            PhGetGeneralCallback(GeneralCallbackNetworkTreeNewInitializing),
            NetworkTreeNewInitializingCallback,
            NULL,
            &NetworkTreeNewInitializingCallbackRegistration
            );
    }

    return TRUE;
}
