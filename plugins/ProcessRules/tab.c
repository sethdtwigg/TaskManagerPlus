/*
 * TaskManagerPlus ProcessRules plugin - Rules tab
 */

#include "processrules.h"

typedef enum _TMP_COLUMN
{
    TMP_COLUMN_PROGRAM,
    TMP_COLUMN_ENABLED,
    TMP_COLUMN_WATCHDOGS,
    TMP_COLUMN_ONSTART,
    TMP_COLUMN_EVENTS,
    TMP_COLUMN_LASTEVENT,
    TMP_COLUMN_PATH,
    TMP_COLUMN_MAXIMUM
} TMP_COLUMN;

typedef enum _TMP_COMMAND
{
    TMP_COMMAND_NEW = 1,
    TMP_COMMAND_EDIT,
    TMP_COMMAND_DELETE,
    TMP_COMMAND_TOGGLE,
    TMP_COMMAND_PAUSE,
    TMP_COMMAND_MUTE,
    TMP_COMMAND_EXPORT,
    TMP_COMMAND_IMPORT,
    TMP_COMMAND_COPY
} TMP_COMMAND;

static HWND TmpTreeNewHandle = NULL;
static BOOLEAN TmpTreeNewCreated = FALSE;
static ULONG TmpSortColumn = TMP_COLUMN_PROGRAM;
static PH_SORT_ORDER TmpSortOrder = AscendingSortOrder;
static PPH_MAIN_TAB_PAGE TmpTabPage = NULL;
static CONST PH_STRINGREF TmpTabPageName = PH_STRINGREF_INIT(L"Rules");
static CONST PH_STRINGREF TmpEmptyText = PH_STRINGREF_INIT(L"No rules. Right-click to create one, or use Rules on a process' context menu.");
static CONST PH_STRINGREF TmpPausedText = PH_STRINGREF_INIT(L"All rules are paused. Right-click and choose Resume all rules.");

static PPH_STRING TmpGetEventsText(
    _In_ PTMP_RULE Rule
    )
{
    if (!Rule->DisplayEvents)
        Rule->DisplayEvents = PhFormatUInt64(Rule->EventCount, TRUE);

    return Rule->DisplayEvents;
}

static PPH_STRING TmpGetLastEventText(
    _In_ PTMP_RULE Rule
    )
{
    if (!Rule->DisplayLastEvent && Rule->LastEventTime.QuadPart)
    {
        SYSTEMTIME systemTime;
        PPH_STRING time;

        PhLargeIntegerToLocalSystemTime(&systemTime, &Rule->LastEventTime);
        time = PhFormatDateTime(&systemTime);
        Rule->DisplayLastEvent = PhFormatString(L"%s  %s", PhGetString(time), PhGetStringOrEmpty(Rule->LastEventText));
        PhDereferenceObject(time);
    }

    return Rule->DisplayLastEvent;
}

static PPH_STRING TmpGetWatchdogsText(
    _In_ PTMP_RULE Rule
    )
{
    if (!Rule->DisplayWatchdogs)
    {
        PH_STRING_BUILDER sb;

        PhInitializeStringBuilder(&sb, 64);

        for (ULONG i = 0; i < TMP_MAX_WATCHDOGS; i++)
        {
            PPH_STRING text;

            if (!Rule->Watchdogs[i].Enabled)
                continue;

            text = TmpFormatWatchdog(&Rule->Watchdogs[i]);
            PhAppendStringBuilder(&sb, &text->sr);
            PhAppendStringBuilder2(&sb, L" | ");
            PhDereferenceObject(text);
        }

        if (PhEndsWithString2(sb.String, L" | ", FALSE))
            PhRemoveEndStringBuilder(&sb, 3);

        Rule->DisplayWatchdogs = PhFinalStringBuilderString(&sb);
    }

    return Rule->DisplayWatchdogs;
}

static PPH_STRING TmpGetOnStartText(
    _In_ PTMP_RULE Rule
    )
{
    if (!Rule->DisplayOnStart)
        Rule->DisplayOnStart = TmpFormatOnStart(Rule);

    return Rule->DisplayOnStart;
}

static PCWSTR TmpGetEnabledText(
    _In_ PTMP_RULE Rule
    )
{
    if (Rule->Enabled)
        return TmpIsEnginePaused() ? L"Paused" : L"Enabled";

    return Rule->AutoDisabled ? L"Disabled (guard)" : L"Disabled";
}

#define SORT_FUNCTION(Column) TmpTreeNewCompare##Column
#define BEGIN_SORT_FUNCTION(Column) static int __cdecl TmpTreeNewCompare##Column( \
    _In_ void const* _elem1, \
    _In_ void const* _elem2 \
    ) \
{ \
    PTMP_RULE node1 = *(PTMP_RULE*)_elem1; \
    PTMP_RULE node2 = *(PTMP_RULE*)_elem2; \
    int sortResult = 0;

#define END_SORT_FUNCTION \
    if (sortResult == 0) \
        sortResult = uintcmp(node1->Id, node2->Id); \
    \
    return PhModifySort(sortResult, TmpSortOrder); \
}

BEGIN_SORT_FUNCTION(Program)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->ProgramName, node2->ProgramName, TmpSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Enabled)
{
    sortResult = PhCompareStringZ(TmpGetEnabledText(node1), TmpGetEnabledText(node2), TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Watchdogs)
{
    sortResult = PhCompareStringWithNullSortOrder(TmpGetWatchdogsText(node1), TmpGetWatchdogsText(node2), TmpSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(OnStart)
{
    sortResult = PhCompareStringWithNullSortOrder(TmpGetOnStartText(node1), TmpGetOnStartText(node2), TmpSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Events)
{
    sortResult = uint64cmp(node1->EventCount, node2->EventCount);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(LastEvent)
{
    sortResult = int64cmp(node1->LastEventTime.QuadPart, node2->LastEventTime.QuadPart);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Path)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->MatchPath, node2->MatchPath, TmpSortOrder, TRUE);
}
END_SORT_FUNCTION

static BOOLEAN TmpGetSelectedRules(
    _Out_ PTMP_RULE** Rules,
    _Out_ PULONG NumberOfRules
    )
{
    PPH_LIST list;

    *Rules = NULL;
    *NumberOfRules = 0;

    if (!TmpRulesList)
        return FALSE;

    list = PhCreateList(2);

    for (ULONG i = 0; i < TmpRulesList->Count; i++)
    {
        PTMP_RULE rule = TmpRulesList->Items[i];

        if (rule->Node.Selected)
            PhAddItemList(list, rule);
    }

    if (list->Count)
    {
        *Rules = PhAllocateCopy(list->Items, sizeof(PVOID) * list->Count);
        *NumberOfRules = list->Count;
        PhDereferenceObject(list);
        return TRUE;
    }

    PhDereferenceObject(list);
    return FALSE;
}

VOID TmpRefreshRulesTab(
    VOID
    )
{
    if (!TmpTreeNewCreated)
        return;

    TreeNew_SetEmptyText(TmpTreeNewHandle, TmpIsEnginePaused() ? &TmpPausedText : &TmpEmptyText, 0);
    TreeNew_NodesStructured(TmpTreeNewHandle);
    InvalidateRect(TmpTreeNewHandle, NULL, FALSE);
}

static VOID TmpCreateNewRule(
    _In_ HWND WindowHandle,
    _In_opt_ PPH_PROCESS_ITEM ProcessItem
    )
{
    PTMP_RULE rule;

    if (TmpRulesList && TmpRulesList->Count >= TMP_MAX_RULES)
    {
        PhShowError2(WindowHandle, L"Rule limit reached", L"At most %u rules are supported.", TMP_MAX_RULES);
        return;
    }

    rule = TmpCreateRule();

    if (ProcessItem && ProcessItem->ProcessName)
        PhSwapReference(&rule->ProgramName, ProcessItem->ProcessName);

    if (TmpShowRuleDialog(WindowHandle, rule))
    {
        TmpAddRule(rule);
        TmpRulesChanged();
    }
    else
    {
        TmpDeleteRule(rule);
    }
}

static VOID TmpEditRule(
    _In_ HWND WindowHandle,
    _In_ PTMP_RULE Rule
    )
{
    if (TmpShowRuleDialog(WindowHandle, Rule))
        TmpRulesChanged();
}

/**
 * Opens the editor for the rule matching a process, or creates one. Used by the process context menu.
 */
VOID TmpEditRuleForProcess(
    _In_ HWND ParentWindowHandle,
    _In_ PPH_PROCESS_ITEM ProcessItem
    )
{
    PTMP_RULE rule = NULL;

    if (ProcessItem->ProcessName)
        rule = TmpFindRuleByProgramName(&ProcessItem->ProcessName->sr);

    if (rule)
        TmpEditRule(ParentWindowHandle, rule);
    else
        TmpCreateNewRule(ParentWindowHandle, ProcessItem);
}

static VOID TmpExportRules(
    _In_ HWND WindowHandle
    )
{
    static CONST PH_FILETYPE_FILTER filters[] =
    {
        { L"JSON files (*.json)", L"*.json" },
        { L"All files (*.*)", L"*.*" }
    };
    PVOID fileDialog;
    PPH_STRING fileName;
    NTSTATUS status;

    fileDialog = PhCreateSaveFileDialog();
    PhSetFileDialogFilter(fileDialog, (PPH_FILETYPE_FILTER)filters, RTL_NUMBER_OF(filters));
    PhSetFileDialogFileName(fileDialog, L"process-rules.json");

    if (PhShowFileDialog(WindowHandle, fileDialog))
    {
        fileName = PhGetFileDialogFileName(fileDialog);
        status = TmpSaveRulesToFile(&fileName->sr);

        if (!NT_SUCCESS(status))
            PhShowStatus(WindowHandle, L"Unable to export the rules.", status, 0);

        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(fileDialog);
}

static VOID TmpImportRules(
    _In_ HWND WindowHandle
    )
{
    static CONST PH_FILETYPE_FILTER filters[] =
    {
        { L"JSON files (*.json)", L"*.json" },
        { L"All files (*.*)", L"*.*" }
    };
    PVOID fileDialog;
    PPH_STRING fileName;
    NTSTATUS status;

    fileDialog = PhCreateOpenFileDialog();
    PhSetFileDialogFilter(fileDialog, (PPH_FILETYPE_FILTER)filters, RTL_NUMBER_OF(filters));

    if (PhShowFileDialog(WindowHandle, fileDialog))
    {
        fileName = PhGetFileDialogFileName(fileDialog);
        status = TmpLoadRulesFromFile(&fileName->sr, TRUE);

        if (NT_SUCCESS(status))
            TmpRulesChanged();
        else
            PhShowStatus(WindowHandle, L"Unable to import the rules.", status, 0);

        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(fileDialog);
}

static VOID TmpHandleCommand(
    _In_ HWND WindowHandle,
    _In_ ULONG Id
    )
{
    PTMP_RULE* rules;
    ULONG numberOfRules;

    TmpGetSelectedRules(&rules, &numberOfRules);

    switch (Id)
    {
    case TMP_COMMAND_NEW:
        TmpCreateNewRule(WindowHandle, NULL);
        break;
    case TMP_COMMAND_EDIT:
        {
            if (numberOfRules == 1)
                TmpEditRule(WindowHandle, rules[0]);
        }
        break;
    case TMP_COMMAND_DELETE:
        {
            if (numberOfRules == 0)
                break;

            if (PhShowConfirmMessage(
                WindowHandle,
                L"delete",
                numberOfRules == 1 ? L"the selected rule" : L"the selected rules",
                NULL,
                FALSE
                ))
            {
                for (ULONG i = 0; i < numberOfRules; i++)
                {
                    TmpRemoveRule(rules[i]);
                    TmpDeleteRule(rules[i]);
                }

                TmpRulesChanged();
            }
        }
        break;
    case TMP_COMMAND_TOGGLE:
        {
            if (numberOfRules == 0)
                break;

            for (ULONG i = 0; i < numberOfRules; i++)
            {
                rules[i]->Enabled = !rules[i]->Enabled;
                rules[i]->AutoDisabled = FALSE;
            }

            TmpRulesChanged();
        }
        break;
    case TMP_COMMAND_PAUSE:
        TmpSetEnginePaused(!TmpIsEnginePaused());
        break;
    case TMP_COMMAND_MUTE:
        {
            if (TmpIsMuted())
                TmpUnmuteNotifications();
            else
                TmpMuteNotifications(TMP_MUTE_SECONDS);
        }
        break;
    case TMP_COMMAND_EXPORT:
        TmpExportRules(WindowHandle);
        break;
    case TMP_COMMAND_IMPORT:
        TmpImportRules(WindowHandle);
        break;
    case TMP_COMMAND_COPY:
        {
            PPH_STRING text;

            text = PhGetTreeNewText(TmpTreeNewHandle, 0);
            PhSetClipboardString(TmpTreeNewHandle, &text->sr);
            PhDereferenceObject(text);
        }
        break;
    }

    if (rules)
        PhFree(rules);
}

static VOID TmpShowContextMenu(
    _In_ HWND WindowHandle,
    _In_ PPH_TREENEW_CONTEXT_MENU ContextMenuEvent
    )
{
    PTMP_RULE* rules;
    ULONG numberOfRules;
    PPH_EMENU menu;
    PPH_EMENU_ITEM item;
    PPH_EMENU_ITEM editItem;
    PPH_EMENU_ITEM deleteItem;
    PPH_EMENU_ITEM toggleItem;
    PPH_EMENU_ITEM muteItem;

    TmpGetSelectedRules(&rules, &numberOfRules);

    menu = PhCreateEMenu();
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TMP_COMMAND_NEW, L"&New rule...", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, editItem = PhCreateEMenuItem(0, TMP_COMMAND_EDIT, L"&Edit rule...\bEnter", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, deleteItem = PhCreateEMenuItem(0, TMP_COMMAND_DELETE, L"&Delete\bDel", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, toggleItem = PhCreateEMenuItem(0, TMP_COMMAND_TOGGLE, L"Ena&ble/disable", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TMP_COMMAND_PAUSE, TmpIsEnginePaused() ? L"&Resume all rules" : L"&Pause all rules", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, muteItem = PhCreateEMenuItem(0, TMP_COMMAND_MUTE, L"&Mute notifications for 1 hour", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TMP_COMMAND_IMPORT, L"&Import rules...", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TMP_COMMAND_EXPORT, L"E&xport rules...", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, TMP_COMMAND_COPY, L"&Copy\bCtrl+C", NULL, NULL), ULONG_MAX);
    PhInsertCopyCellEMenuItem(menu, TMP_COMMAND_COPY, WindowHandle, ContextMenuEvent->Column);
    PhSetFlagsEMenuItem(menu, TMP_COMMAND_EDIT, PH_EMENU_DEFAULT, PH_EMENU_DEFAULT);

    if (numberOfRules != 1)
        PhSetDisabledEMenuItem(editItem);
    if (numberOfRules == 0)
    {
        PhSetDisabledEMenuItem(deleteItem);
        PhSetDisabledEMenuItem(toggleItem);
    }
    if (TmpIsMuted())
        muteItem->Flags |= PH_EMENU_CHECKED;

    item = PhShowEMenu(
        menu,
        WindowHandle,
        PH_EMENU_SHOW_LEFTRIGHT,
        PH_ALIGN_LEFT | PH_ALIGN_TOP,
        ContextMenuEvent->Location.x,
        ContextMenuEvent->Location.y
        );

    if (item)
    {
        if (!PhHandleCopyCellEMenuItem(item))
            TmpHandleCommand(WindowHandle, item->Id);
    }

    PhDestroyEMenu(menu);

    if (rules)
        PhFree(rules);
}

static BOOLEAN NTAPI TmpTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_opt_ PVOID Context
    )
{
    PTMP_RULE node;

    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;

            if (getChildren->Node)
                return FALSE;

            if (!TmpRulesList)
            {
                getChildren->Children = NULL;
                getChildren->NumberOfChildren = 0;
                return TRUE;
            }

            if (TmpSortOrder != NoSortOrder)
            {
                static CONST _CoreCrtNonSecureSearchSortCompareFunction sortFunctions[] =
                {
                    SORT_FUNCTION(Program),
                    SORT_FUNCTION(Enabled),
                    SORT_FUNCTION(Watchdogs),
                    SORT_FUNCTION(OnStart),
                    SORT_FUNCTION(Events),
                    SORT_FUNCTION(LastEvent),
                    SORT_FUNCTION(Path),
                };

                static_assert(RTL_NUMBER_OF(sortFunctions) == TMP_COLUMN_MAXIMUM, "SortFunctions must equal maximum.");

                if (TmpSortColumn < TMP_COLUMN_MAXIMUM)
                    qsort(TmpRulesList->Items, TmpRulesList->Count, sizeof(PVOID), sortFunctions[TmpSortColumn]);
            }

            getChildren->Children = (PPH_TREENEW_NODE*)TmpRulesList->Items;
            getChildren->NumberOfChildren = TmpRulesList->Count;
        }
        return TRUE;
    case TreeNewIsLeaf:
        {
            PPH_TREENEW_IS_LEAF isLeaf = Parameter1;

            isLeaf->IsLeaf = TRUE;
        }
        return TRUE;
    case TreeNewGetCellText:
        {
            PPH_TREENEW_GET_CELL_TEXT getCellText = Parameter1;

            node = (PTMP_RULE)getCellText->Node;

            switch (getCellText->Id)
            {
            case TMP_COLUMN_PROGRAM:
                getCellText->Text = PhGetStringRef(node->ProgramName);
                break;
            case TMP_COLUMN_ENABLED:
                PhInitializeStringRefLongHint(&getCellText->Text, TmpGetEnabledText(node));
                break;
            case TMP_COLUMN_WATCHDOGS:
                getCellText->Text = PhGetStringRef(TmpGetWatchdogsText(node));
                break;
            case TMP_COLUMN_ONSTART:
                getCellText->Text = PhGetStringRef(TmpGetOnStartText(node));
                break;
            case TMP_COLUMN_EVENTS:
                getCellText->Text = PhGetStringRef(TmpGetEventsText(node));
                break;
            case TMP_COLUMN_LASTEVENT:
                getCellText->Text = PhGetStringRef(TmpGetLastEventText(node));
                break;
            case TMP_COLUMN_PATH:
                getCellText->Text = PhGetStringRef(node->MatchPath);
                break;
            default:
                return FALSE;
            }

            getCellText->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewGetNodeColor:
        {
            PPH_TREENEW_GET_NODE_COLOR getNodeColor = Parameter1;

            node = (PTMP_RULE)getNodeColor->Node;

            if (!node->Enabled || TmpIsEnginePaused())
                getNodeColor->ForeColor = GetSysColor(COLOR_GRAYTEXT);

            getNodeColor->Flags = TN_AUTO_FORECOLOR;
        }
        return TRUE;
    case TreeNewSortChanged:
        {
            PPH_TREENEW_SORT_CHANGED_EVENT sorting = Parameter1;

            TmpSortColumn = sorting->SortColumn;
            TmpSortOrder = sorting->SortOrder;

            TreeNew_NodesStructured(WindowHandle);
        }
        return TRUE;
    case TreeNewKeyDown:
        {
            PPH_TREENEW_KEY_EVENT keyEvent = Parameter1;

            switch (keyEvent->VirtualKey)
            {
            case 'C':
                if (GetKeyState(VK_CONTROL) < 0)
                    TmpHandleCommand(WindowHandle, TMP_COMMAND_COPY);
                break;
            case VK_DELETE:
                TmpHandleCommand(WindowHandle, TMP_COMMAND_DELETE);
                break;
            case VK_RETURN:
                TmpHandleCommand(WindowHandle, TMP_COMMAND_EDIT);
                break;
            }
        }
        return TRUE;
    case TreeNewHeaderRightClick:
        {
            PH_TN_COLUMN_MENU_DATA data;

            data.TreeNewHandle = WindowHandle;
            data.MouseEvent = Parameter1;
            data.DefaultSortColumn = TMP_COLUMN_PROGRAM;
            data.DefaultSortOrder = AscendingSortOrder;
            PhInitializeTreeNewColumnMenuEx(&data, PH_TN_COLUMN_MENU_SHOW_RESET_SORT);

            data.Selection = PhShowEMenu(
                data.Menu,
                WindowHandle,
                PH_EMENU_SHOW_LEFTRIGHT,
                PH_ALIGN_LEFT | PH_ALIGN_TOP,
                data.MouseEvent->ScreenLocation.x,
                data.MouseEvent->ScreenLocation.y
                );
            PhHandleTreeNewColumnMenu(&data);
            PhDeleteTreeNewColumnMenu(&data);
        }
        return TRUE;
    case TreeNewLeftDoubleClick:
        {
            TmpHandleCommand(WindowHandle, TMP_COMMAND_EDIT);
        }
        return TRUE;
    case TreeNewContextMenu:
        {
            TmpShowContextMenu(WindowHandle, Parameter1);
        }
        return TRUE;
    }

    return FALSE;
}

static VOID TmpLoadSettingsTreeList(
    _In_ HWND TreeNewHandle
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;

    settings = PhGetStringSetting(SETTING_NAME_RULES_COLUMNS);
    PhCmLoadSettings(TreeNewHandle, &settings->sr);
    PhDereferenceObject(settings);

    sortSettings = PhGetIntegerPairSetting(SETTING_NAME_RULES_SORT);
    TreeNew_SetSort(TreeNewHandle, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);
}

static VOID TmpSaveSettingsTreeList(
    _In_ HWND TreeNewHandle
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;
    ULONG sortColumn;
    PH_SORT_ORDER sortOrder;

    if (!TmpTreeNewCreated)
        return;

    settings = PhCmSaveSettings(TreeNewHandle);
    PhSetStringSetting2(SETTING_NAME_RULES_COLUMNS, &settings->sr);
    PhDereferenceObject(settings);

    TreeNew_GetSort(TreeNewHandle, &sortColumn, &sortOrder);
    sortSettings.X = sortColumn;
    sortSettings.Y = sortOrder;
    PhSetIntegerPairSetting(SETTING_NAME_RULES_SORT, sortSettings);
}

static VOID TmpInitializeTreeList(
    _In_ HWND TreeNewHandle
    )
{
    PhSetControlTheme(TreeNewHandle, L"explorer");
    TreeNew_SetRedraw(TreeNewHandle, FALSE);
    TreeNew_SetCallback(TreeNewHandle, TmpTreeNewCallback, NULL);

    PhAddTreeNewColumn(TreeNewHandle, TMP_COLUMN_PROGRAM, TRUE, L"Program", 140, PH_ALIGN_LEFT, TMP_COLUMN_PROGRAM, 0);
    PhAddTreeNewColumn(TreeNewHandle, TMP_COLUMN_ENABLED, TRUE, L"Status", 90, PH_ALIGN_LEFT, TMP_COLUMN_ENABLED, 0);
    PhAddTreeNewColumn(TreeNewHandle, TMP_COLUMN_ONSTART, TRUE, L"On start", 200, PH_ALIGN_LEFT, TMP_COLUMN_ONSTART, 0);
    PhAddTreeNewColumn(TreeNewHandle, TMP_COLUMN_WATCHDOGS, TRUE, L"Watchdogs", 260, PH_ALIGN_LEFT, TMP_COLUMN_WATCHDOGS, 0);
    PhAddTreeNewColumnEx(TreeNewHandle, TMP_COLUMN_EVENTS, TRUE, L"Events", 50, PH_ALIGN_RIGHT, TMP_COLUMN_EVENTS, DT_RIGHT, TRUE);
    PhAddTreeNewColumn(TreeNewHandle, TMP_COLUMN_LASTEVENT, TRUE, L"Last event", 260, PH_ALIGN_LEFT, TMP_COLUMN_LASTEVENT, 0);
    PhAddTreeNewColumn(TreeNewHandle, TMP_COLUMN_PATH, TRUE, L"Path filter", 160, PH_ALIGN_LEFT, TMP_COLUMN_PATH, DT_PATH_ELLIPSIS);

    if (PhGetIntegerSetting(L"TreeListCustomRowSize"))
    {
        ULONG rowSize = PhGetIntegerSetting(L"TreeListCustomRowSize");

        if (rowSize < 15)
            rowSize = 15;

        TreeNew_SetRowHeight(TreeNewHandle, rowSize);
    }

    TreeNew_SetSort(TreeNewHandle, TMP_COLUMN_PROGRAM, AscendingSortOrder);
    TreeNew_SetTriState(TreeNewHandle, TRUE);
    TreeNew_SetEmptyText(TreeNewHandle, &TmpEmptyText, 0);
    TreeNew_SetRedraw(TreeNewHandle, TRUE);

    TmpLoadSettingsTreeList(TreeNewHandle);
}

_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
static BOOLEAN TmpTabPageCallback(
    _In_ PPH_MAIN_TAB_PAGE Page,
    _In_ PH_MAIN_TAB_PAGE_MESSAGE Message,
    _In_opt_ PVOID Parameter1,
    _In_opt_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case MainTabPageCreateWindow:
        {
            ULONG thinRows;
            ULONG treelistBorder;
            ULONG treelistCustomColors;
            PH_TREENEW_CREATEPARAMS treelistCreateParams = { sizeof(PH_TREENEW_CREATEPARAMS) };
            RECT clientRect = { 0 };

            thinRows = PhGetIntegerSetting(L"ThinRows") ? TN_STYLE_THIN_ROWS : 0;
            treelistBorder = (PhGetIntegerSetting(L"TreeListBorderEnable") && !PhGetIntegerSetting(L"EnableThemeSupport")) ? WS_BORDER : 0;
            treelistCustomColors = PhGetIntegerSetting(L"TreeListCustomColorsEnable") ? TN_STYLE_CUSTOM_COLORS : 0;

            if (treelistCustomColors)
            {
                treelistCreateParams.TextColor = PhGetIntegerSetting(L"TreeListCustomColorText");
                treelistCreateParams.FocusColor = PhGetIntegerSetting(L"TreeListCustomColorFocus");
                treelistCreateParams.SelectionColor = PhGetIntegerSetting(L"TreeListCustomColorSelection");
            }

            PhGetClientRect(Parameter2, &clientRect);

            TmpTreeNewHandle = PhCreateWindow(
                PH_TREENEW_CLASSNAME,
                NULL,
                WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | TN_STYLE_ICONS | TN_STYLE_DOUBLE_BUFFERED | thinRows | treelistBorder | treelistCustomColors,
                clientRect.left,
                clientRect.top,
                clientRect.right - clientRect.left,
                clientRect.bottom - clientRect.top,
                Parameter2,
                NULL,
                NULL,
                &treelistCreateParams
                );

            if (!TmpTreeNewHandle)
                return FALSE;

            TmpTreeNewCreated = TRUE;

            if (PhGetIntegerSetting(L"EnableThemeSupport"))
            {
                PhInitializeWindowTheme(TmpTreeNewHandle, TRUE);
                TreeNew_ThemeSupport(TmpTreeNewHandle, TRUE);
            }

            TmpInitializeTreeList(TmpTreeNewHandle);
            TmpRefreshRulesTab();

            if (Parameter1)
                *(HWND*)Parameter1 = TmpTreeNewHandle;
        }
        return TRUE;
    case MainTabPageSaveSettings:
        {
            TmpSaveSettingsTreeList(TmpTreeNewHandle);
        }
        return TRUE;
    case MainTabPageSelected:
        {
            if (PtrToUlong(Parameter1))
                TmpRefreshRulesTab();
        }
        break;
    case MainTabPageExportContent:
        {
            PPH_MAIN_TAB_PAGE_EXPORT_CONTENT exportContent = Parameter1;
            PPH_LIST lines;

            if (!exportContent || !TmpTreeNewCreated)
                return FALSE;

            lines = PhGetGenericTreeNewLines(TmpTreeNewHandle, exportContent->Mode);

            for (ULONG i = 0; i < lines->Count; i++)
            {
                PPH_STRING line = lines->Items[i];

                PhWriteStringAsUtf8FileStream(exportContent->FileStream, &line->sr);
                PhDereferenceObject(line);
                PhWriteStringAsUtf8FileStream2(exportContent->FileStream, L"\r\n");
            }

            PhDereferenceObject(lines);
        }
        return TRUE;
    case MainTabPageFontChanged:
        {
            if (TmpTreeNewHandle)
                SendMessage(TmpTreeNewHandle, WM_SETFONT, (WPARAM)Parameter1, TRUE);
        }
        break;
    }

    return FALSE;
}

VOID TmpInitializeRulesTab(
    VOID
    )
{
    PH_MAIN_TAB_PAGE page;

    memset(&page, 0, sizeof(PH_MAIN_TAB_PAGE));
    page.Name = TmpTabPageName;
    page.Callback = TmpTabPageCallback;
    TmpTabPage = PhPluginCreateTabPage(&page);
}
