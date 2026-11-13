/*
 * TaskManagerPlus: generic "commands" interface. A plugin that wants entries in the QuickActions palette (Ctrl+K)
 * exports one of these (SessionReports, Profiles); the palette lists every command and calls Run with the id.
 *
 * Usage:
 *   PTMP_COMMANDS_INTERFACE intf = PhGetPluginInterfaceZ(L"TaskManagerPlus.Profiles", TMP_COMMANDS_INTERFACE_VERSION);
 *   PPH_LIST commands = intf->Enumerate();   // PTMP_COMMAND_INFO items, main thread
 *   intf->Run(command->Id, hwnd);
 *   intf->FreeCommands(commands);
 */

#ifndef _COMMANDSINTF_H
#define _COMMANDSINTF_H

#define TMP_COMMANDS_INTERFACE_VERSION 1
#define TMP_COMMANDS_REPORTS_PLUGIN_NAME L"TaskManagerPlus.SessionReports"
#define TMP_COMMANDS_PROFILES_PLUGIN_NAME L"TaskManagerPlus.Profiles"
#define TMP_COMMANDS_ADVISOR_PLUGIN_NAME L"TaskManagerPlus.EfficiencyAdvisor"

typedef struct _TMP_COMMAND_INFO
{
    ULONG Id;
    PPH_STRING Label; // "Export processes as CSV", "Switch to profile Work"
    PPH_STRING Detail; // right-aligned grey text, may be NULL
    PCWSTR Alias; // extra search words (static string), may be NULL
} TMP_COMMAND_INFO, *PTMP_COMMAND_INFO;

typedef struct _TMP_COMMANDS_INTERFACE
{
    ULONG Version;
    PPH_LIST (NTAPI *Enumerate)(VOID); // main thread; free with FreeCommands
    VOID (NTAPI *FreeCommands)(_In_ PPH_LIST Commands);
    VOID (NTAPI *Run)(_In_ ULONG Id, _In_ HWND OwnerWindow); // main thread
} TMP_COMMANDS_INTERFACE, *PTMP_COMMANDS_INTERFACE;

#endif
