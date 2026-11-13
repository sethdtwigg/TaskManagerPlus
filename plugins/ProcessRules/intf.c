/*
 * TaskManagerPlus ProcessRules: interface for the Profiles and Efficiency advisor plugins.
 */

#include "processrules.h"
#include <processrulesintf.h>

static PPH_LIST NTAPI TmpIntfEnumerate(
    VOID
    )
{
    PPH_LIST list = PhCreateList(16);
    ULONG i;

    if (!TmpRulesList)
        return list;

    for (i = 0; i < TmpRulesList->Count; i++)
    {
        PTMP_RULE rule = TmpRulesList->Items[i];
        PPROCESS_RULE_INFO info = PhAllocateZero(sizeof(PROCESS_RULE_INFO));

        info->Id = rule->Id;
        PhSetReference(&info->ProgramName, rule->ProgramName);
        info->Enabled = rule->Enabled;
        info->StartPriority = rule->StartPriority;
        info->StartEfficiency = rule->StartEfficiencyMode;
        PhAddItemList(list, info);
    }

    return list;
}

static VOID NTAPI TmpIntfFreeRules(
    _In_ PPH_LIST Rules
    )
{
    ULONG i;

    for (i = 0; i < Rules->Count; i++)
    {
        PPROCESS_RULE_INFO info = Rules->Items[i];

        PhClearReference(&info->ProgramName);
        PhFree(info);
    }

    PhDereferenceObject(Rules);
}

static BOOLEAN NTAPI TmpIntfSetRuleState(
    _In_ PPH_STRING ProgramName,
    _In_ ULONG EnabledMode,
    _In_ BOOLEAN SetPriority,
    _In_ ULONG Priority
    )
{
    PTMP_RULE rule;

    if (!ProgramName || !(rule = TmpFindRuleByProgramName(&ProgramName->sr)))
        return FALSE;

    if (EnabledMode == PROCESS_RULES_SET_ON)
    {
        rule->Enabled = TRUE;
        rule->AutoDisabled = FALSE;
    }
    else if (EnabledMode == PROCESS_RULES_SET_OFF)
    {
        rule->Enabled = FALSE;
    }

    if (SetPriority)
        rule->StartPriority = Priority;

    return TRUE;
}

static VOID NTAPI TmpIntfCommit(
    VOID
    )
{
    TmpRulesChanged();
}

static VOID NTAPI TmpIntfEditRuleForProcess(
    _In_ HWND OwnerWindow,
    _In_ PPH_PROCESS_ITEM ProcessItem,
    _In_ ULONG Priority,
    _In_ BOOLEAN Efficiency
    )
{
    PTMP_RULE rule;

    if (!ProcessItem->ProcessName)
        return;

    if (rule = TmpFindRuleByProgramName(&ProcessItem->ProcessName->sr))
    {
        if (TmpShowRuleDialog(OwnerWindow, rule))
            TmpRulesChanged();

        return;
    }

    if (TmpRulesList && TmpRulesList->Count >= TMP_MAX_RULES)
    {
        PhShowError2(OwnerWindow, L"Rule limit reached", L"At most %u rules are supported.", TMP_MAX_RULES);
        return;
    }

    rule = TmpCreateRule();
    PhSwapReference(&rule->ProgramName, ProcessItem->ProcessName);

    if (Priority)
        rule->StartPriority = Priority;

    if (Efficiency)
        rule->StartEfficiencyMode = TRUE;

    if (TmpShowRuleDialog(OwnerWindow, rule))
    {
        TmpAddRule(rule);
        TmpRulesChanged();
    }
    else
    {
        TmpDeleteRule(rule);
    }
}

PROCESS_RULES_INTERFACE TmpInterface =
{
    PROCESS_RULES_INTERFACE_VERSION,
    TmpIntfEnumerate,
    TmpIntfFreeRules,
    TmpIntfSetRuleState,
    TmpIntfCommit,
    TmpIntfEditRuleForProcess
};
