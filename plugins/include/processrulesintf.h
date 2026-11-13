/*
 * TaskManagerPlus: interface exported by the ProcessRules plugin (used by Profiles and the Efficiency advisor).
 * All functions run on the main thread.
 *
 *   PPROCESS_RULES_INTERFACE intf = PhGetPluginInterfaceZ(PROCESS_RULES_PLUGIN_NAME, PROCESS_RULES_INTERFACE_VERSION);
 *   PPH_LIST rules = intf->Enumerate();              // PPROCESS_RULE_INFO items
 *   intf->SetRuleState(name, PROCESS_RULES_SET_OFF, FALSE, 0);   // then once: intf->Commit();
 *   intf->FreeRules(rules);
 */

#ifndef _PROCESSRULESINTF_H
#define _PROCESSRULESINTF_H

#define PROCESS_RULES_PLUGIN_NAME L"TaskManagerPlus.ProcessRules"
#define PROCESS_RULES_INTERFACE_VERSION 1

#define PROCESS_RULES_KEEP 0 // SetRuleState EnabledMode: leave the enabled state alone
#define PROCESS_RULES_SET_ON 1
#define PROCESS_RULES_SET_OFF 2

typedef struct _PROCESS_RULE_INFO
{
    ULONG Id;
    PPH_STRING ProgramName; // image base name, the identity used by profiles
    BOOLEAN Enabled;
    ULONG StartPriority; // 0 = no change, else PROCESS_PRIORITY_CLASS_*
    BOOLEAN StartEfficiency;
} PROCESS_RULE_INFO, *PPROCESS_RULE_INFO;

typedef struct _PROCESS_RULES_INTERFACE
{
    ULONG Version;
    PPH_LIST (NTAPI *Enumerate)(VOID);
    VOID (NTAPI *FreeRules)(_In_ PPH_LIST Rules);
    /**
     * EnabledMode is PROCESS_RULES_KEEP / SET_ON / SET_OFF. When SetPriority is TRUE the rule's start priority becomes
     * Priority (0 = the rule does not change priority, else a PROCESS_PRIORITY_CLASS_*). FALSE when no rule has that program name.
     */
    BOOLEAN (NTAPI *SetRuleState)(_In_ PPH_STRING ProgramName, _In_ ULONG EnabledMode, _In_ BOOLEAN SetPriority, _In_ ULONG Priority);
    /** Saves the rules file and refreshes the Rules tab after a batch of SetRuleState calls. */
    VOID (NTAPI *Commit)(VOID);
    /** Opens the rule dialog for the process (creates a rule pre-filled with the given start priority / efficiency mode when none exists). */
    VOID (NTAPI *EditRuleForProcess)(_In_ HWND OwnerWindow, _In_ PPH_PROCESS_ITEM ProcessItem, _In_ ULONG Priority, _In_ BOOLEAN Efficiency);
} PROCESS_RULES_INTERFACE, *PPROCESS_RULES_INTERFACE;

#endif
