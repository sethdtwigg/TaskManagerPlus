/*
 * TaskManagerPlus StartupManager plugin.
 *
 * The "Startup" main-window tab: a TreeNew list of startup entries with
 * sorting, column persistence, a context menu and keyboard shortcuts.
 * Everything here runs on the main thread.
 */

#include "startupmanager.h"

typedef enum _SM_COMMAND_ID
{
    SM_COMMAND_ID_ENABLE = 1,
    SM_COMMAND_ID_DISABLE,
    SM_COMMAND_ID_OPEN_FILE_LOCATION,
    SM_COMMAND_ID_OPEN_SOURCE,
    SM_COMMAND_ID_DELETE,
    SM_COMMAND_ID_COPY,
    SM_COMMAND_ID_REFRESH,
} SM_COMMAND_ID;

static CONST PH_STRINGREF SmTabPageName = PH_STRINGREF_INIT(L"Startup");
static CONST PH_STRINGREF SmEmptyText = PH_STRINGREF_INIT(L"No startup entries were found.");
static CONST PH_STRINGREF SmStatusEnabled = PH_STRINGREF_INIT(L"Enabled");
static CONST PH_STRINGREF SmStatusDisabled = PH_STRINGREF_INIT(L"Disabled");

static PPH_MAIN_TAB_PAGE SmAddedTabPage = NULL;
static HWND SmTreeNewHandle = NULL;
static BOOLEAN SmTreeNewCreated = FALSE;
static BOOLEAN SmRefreshPending = FALSE;
static ULONG SmTreeNewSortColumn = SM_COLUMN_NAME;
static PH_SORT_ORDER SmTreeNewSortOrder = AscendingSortOrder;
static PPH_LIST SmNodeList = NULL;

static VOID SmRefreshStartupList(
    VOID
    );

static VOID SmHandleCommand(
    _In_ HWND TreeWindowHandle,
    _In_ ULONG Id
    );

// Settings

static VOID SmLoadSettingsTreeList(
    _In_ HWND TreeNewHandle
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;

    settings = PhGetStringSetting(SETTING_NAME_TREE_LIST_COLUMNS);
    PhCmLoadSettings(TreeNewHandle, &settings->sr);
    PhDereferenceObject(settings);

    sortSettings = PhGetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT);
    TreeNew_SetSort(TreeNewHandle, (ULONG)sortSettings.X, (PH_SORT_ORDER)sortSettings.Y);
}

static VOID SmSaveSettingsTreeList(
    _In_ HWND TreeNewHandle
    )
{
    PPH_STRING settings;
    PH_INTEGER_PAIR sortSettings;
    ULONG sortColumn;
    PH_SORT_ORDER sortOrder;

    if (!SmTreeNewCreated)
        return;

    settings = PhCmSaveSettings(TreeNewHandle);
    PhSetStringSetting2(SETTING_NAME_TREE_LIST_COLUMNS, &settings->sr);
    PhDereferenceObject(settings);

    TreeNew_GetSort(TreeNewHandle, &sortColumn, &sortOrder);
    sortSettings.X = sortColumn;
    sortSettings.Y = sortOrder;
    PhSetIntegerPairSetting(SETTING_NAME_TREE_LIST_SORT, sortSettings);
}

// Nodes

static VOID SmClearNodeList(
    VOID
    )
{
    for (ULONG i = 0; i < SmNodeList->Count; i++)
    {
        SmFreeStartupItem(SmNodeList->Items[i]);
    }

    PhClearList(SmNodeList);
}

static VOID SmRefreshStartupList(
    VOID
    )
{
    if (!SmTreeNewHandle)
    {
        SmRefreshPending = TRUE;
        return;
    }

    SmRefreshPending = FALSE;

    TreeNew_SetRedraw(SmTreeNewHandle, FALSE);

    SmClearNodeList();
    SmEnumerateStartupItems(SmNodeList);

    for (ULONG i = 0; i < SmNodeList->Count; i++)
    {
        PSM_STARTUP_ITEM item = SmNodeList->Items[i];

        PhInitializeTreeNewNode(&item->Node);
        memset(item->TextCache, 0, sizeof(PH_STRINGREF) * SM_COLUMN_MAXIMUM);
        item->Node.TextCache = item->TextCache;
        item->Node.TextCacheSize = SM_COLUMN_MAXIMUM;
    }

    TreeNew_NodesStructured(SmTreeNewHandle);
    TreeNew_SetRedraw(SmTreeNewHandle, TRUE);
}

_Success_(return)
static BOOLEAN SmGetSelectedItems(
    _Out_ PSM_STARTUP_ITEM** Items,
    _Out_ PULONG NumberOfItems
    )
{
    PPH_LIST list = PhCreateList(2);

    for (ULONG i = 0; i < SmNodeList->Count; i++)
    {
        PSM_STARTUP_ITEM node = SmNodeList->Items[i];

        if (node->Node.Selected)
        {
            PhAddItemList(list, node);
        }
    }

    if (list->Count)
    {
        *Items = PhAllocateCopy(list->Items, sizeof(PVOID) * list->Count);
        *NumberOfItems = list->Count;

        PhDereferenceObject(list);
        return TRUE;
    }

    PhDereferenceObject(list);
    return FALSE;
}

static PSM_STARTUP_ITEM SmGetSelectedItem(
    VOID
    )
{
    for (ULONG i = 0; i < SmNodeList->Count; i++)
    {
        PSM_STARTUP_ITEM node = SmNodeList->Items[i];

        if (node->Node.Selected)
            return node;
    }

    return NULL;
}

// Sorting

#define SORT_FUNCTION(Column) SmTreeNewCompare##Column
#define BEGIN_SORT_FUNCTION(Column) static int __cdecl SmTreeNewCompare##Column( \
    _In_ void const* _elem1, \
    _In_ void const* _elem2 \
    ) \
{ \
    PSM_STARTUP_ITEM node1 = *(PSM_STARTUP_ITEM*)_elem1; \
    PSM_STARTUP_ITEM node2 = *(PSM_STARTUP_ITEM*)_elem2; \
    int sortResult = 0;

#define END_SORT_FUNCTION \
    if (sortResult == 0) \
        sortResult = uintcmp(node1->Index, node2->Index); \
    \
    return PhModifySort(sortResult, SmTreeNewSortOrder); \
}

BEGIN_SORT_FUNCTION(Name)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->Name, node2->Name, SmTreeNewSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Status)
{
    sortResult = intcmp(node1->Enabled, node2->Enabled);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Type)
{
    sortResult = PhCompareStringRef(SmGetSourceTypeString(node1), SmGetSourceTypeString(node2), TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Publisher)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->Publisher, node2->Publisher, SmTreeNewSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Command)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->Command, node2->Command, SmTreeNewSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Location)
{
    sortResult = PhCompareStringWithNullSortOrder(node1->Location, node2->Location, SmTreeNewSortOrder, TRUE);
}
END_SORT_FUNCTION

BEGIN_SORT_FUNCTION(Impact)
{
    sortResult = uint64cmp(node1->FileSize, node2->FileSize);
}
END_SORT_FUNCTION

static int __cdecl SmTreeNewNoOrderSortFunction(
    _In_ void const* _elem1,
    _In_ void const* _elem2
    )
{
    PSM_STARTUP_ITEM node1 = *(PSM_STARTUP_ITEM*)_elem1;
    PSM_STARTUP_ITEM node2 = *(PSM_STARTUP_ITEM*)_elem2;

    return uintcmp(node1->Index, node2->Index);
}

// TaskManagerPlus: disabled-row text derived from the active theme palette (halfway between
// the palette text colour and the palette background), so it works in light and dark.
static COLORREF SmDisabledTextColor(
    _In_ HWND WindowHandle
    )
{
    COLORREF color = GetSysColor(COLOR_GRAYTEXT);

    if (PhGetIntegerSetting(L"EnableThemeSupport"))
    {
        HDC hdc = CreateCompatibleDC(NULL);
        LOGBRUSH brush;

        if (hdc)
        {
            COLORREF text;
            HBRUSH backgroundBrush;

            PhWindowThemeControlColor(WindowHandle, hdc, WindowHandle, CTLCOLOR_STATIC);
            text = GetTextColor(hdc);
            backgroundBrush = PhWindowThemeControlColor(WindowHandle, hdc, WindowHandle, CTLCOLOR_DLG);

            if (backgroundBrush && GetObject(backgroundBrush, sizeof(LOGBRUSH), &brush))
            {

                color = RGB(
                    (GetRValue(text) + GetRValue(brush.lbColor)) / 2,
                    (GetGValue(text) + GetGValue(brush.lbColor)) / 2,
                    (GetBValue(text) + GetBValue(brush.lbColor)) / 2
                    );
            }

            DeleteDC(hdc);
        }
    }

    return color;
}

// TreeNew callback

static BOOLEAN NTAPI SmTreeNewCallback(
    _In_ HWND WindowHandle,
    _In_ PH_TREENEW_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2,
    _In_opt_ PVOID Context
    )
{
    PSM_STARTUP_ITEM node;

    switch (Message)
    {
    case TreeNewGetChildren:
        {
            PPH_TREENEW_GET_CHILDREN getChildren = Parameter1;

            if (getChildren->Node)
                break;

            if (SmTreeNewSortOrder == NoSortOrder)
            {
                qsort(SmNodeList->Items, SmNodeList->Count, sizeof(PVOID), SmTreeNewNoOrderSortFunction);
            }
            else
            {
                static CONST _CoreCrtNonSecureSearchSortCompareFunction sortFunctions[] =
                {
                    SORT_FUNCTION(Name),
                    SORT_FUNCTION(Status),
                    SORT_FUNCTION(Type),
                    SORT_FUNCTION(Publisher),
                    SORT_FUNCTION(Command),
                    SORT_FUNCTION(Location),
                    SORT_FUNCTION(Impact),
                };

                static_assert(RTL_NUMBER_OF(sortFunctions) == SM_COLUMN_MAXIMUM, "SortFunctions must equal maximum.");

                if (SmTreeNewSortColumn < SM_COLUMN_MAXIMUM)
                {
                    qsort(SmNodeList->Items, SmNodeList->Count, sizeof(PVOID), sortFunctions[SmTreeNewSortColumn]);
                }
            }

            getChildren->Children = (PPH_TREENEW_NODE*)SmNodeList->Items;
            getChildren->NumberOfChildren = SmNodeList->Count;
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
            node = (PSM_STARTUP_ITEM)getCellText->Node;

            switch (getCellText->Id)
            {
            case SM_COLUMN_NAME:
                getCellText->Text = PhGetStringRef(node->Name);
                break;
            case SM_COLUMN_STATUS:
                getCellText->Text = node->Enabled ? SmStatusEnabled : SmStatusDisabled;
                break;
            case SM_COLUMN_TYPE:
                getCellText->Text = *SmGetSourceTypeString(node);
                break;
            case SM_COLUMN_PUBLISHER:
                getCellText->Text = PhGetStringRef(node->Publisher);
                break;
            case SM_COLUMN_COMMAND:
                getCellText->Text = PhGetStringRef(node->Command);
                break;
            case SM_COLUMN_LOCATION:
                getCellText->Text = PhGetStringRef(node->Location);
                break;
            case SM_COLUMN_IMPACT:
                getCellText->Text = *SmGetImpactString(node->Impact);
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
            node = (PSM_STARTUP_ITEM)getNodeColor->Node;

            if (!node->Enabled)
            {
                getNodeColor->ForeColor = SmDisabledTextColor(WindowHandle);
            }

            getNodeColor->Flags = TN_CACHE;
        }
        return TRUE;
    case TreeNewSortChanged:
        {
            PPH_TREENEW_SORT_CHANGED_EVENT sorting = Parameter1;

            SmTreeNewSortColumn = sorting->SortColumn;
            SmTreeNewSortOrder = sorting->SortOrder;

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
                    SmHandleCommand(WindowHandle, SM_COMMAND_ID_COPY);
                break;
            case VK_F5:
                SmHandleCommand(WindowHandle, SM_COMMAND_ID_REFRESH);
                break;
            case VK_DELETE:
                SmHandleCommand(WindowHandle, SM_COMMAND_ID_DELETE);
                break;
            case VK_RETURN:
                SmHandleCommand(WindowHandle, SM_COMMAND_ID_OPEN_FILE_LOCATION);
                break;
            }
        }
        return TRUE;
    case TreeNewHeaderRightClick:
        {
            PH_TN_COLUMN_MENU_DATA data;

            data.TreeNewHandle = WindowHandle;
            data.MouseEvent = Parameter1;
            data.DefaultSortColumn = SM_COLUMN_NAME;
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
            SmHandleCommand(WindowHandle, SM_COMMAND_ID_OPEN_FILE_LOCATION);
        }
        return TRUE;
    case TreeNewContextMenu:
        {
            PPH_TREENEW_CONTEXT_MENU contextMenuEvent = Parameter1;
            PSM_STARTUP_ITEM* items;
            ULONG numberOfItems;
            PPH_EMENU menu;
            PPH_EMENU_ITEM item;
            PPH_EMENU_ITEM enableItem;
            PPH_EMENU_ITEM disableItem;
            PPH_EMENU_ITEM openSourceItem;
            PPH_EMENU_ITEM openFileItem;
            PPH_EMENU_ITEM deleteItem;
            BOOLEAN anyEnabled = FALSE;
            BOOLEAN anyDisabled = FALSE;
            BOOLEAN anyToggleable = FALSE;
            BOOLEAN hasSelection;

            hasSelection = SmGetSelectedItems(&items, &numberOfItems);

            if (hasSelection)
            {
                for (ULONG i = 0; i < numberOfItems; i++)
                {
                    if (items[i]->CanToggle)
                    {
                        anyToggleable = TRUE;

                        if (items[i]->Enabled)
                            anyEnabled = TRUE;
                        else
                            anyDisabled = TRUE;
                    }
                }
            }

            menu = PhCreateEMenu();
            PhInsertEMenuItem(menu, enableItem = PhCreateEMenuItem(0, SM_COMMAND_ID_ENABLE, L"&Enable", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, disableItem = PhCreateEMenuItem(0, SM_COMMAND_ID_DISABLE, L"&Disable", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
            PhInsertEMenuItem(menu, openFileItem = PhCreateEMenuItem(0, SM_COMMAND_ID_OPEN_FILE_LOCATION, L"Open &file location\bEnter", NULL, NULL), ULONG_MAX);

            if (hasSelection && items[0]->Source == SmSourceScheduledTask)
                PhInsertEMenuItem(menu, openSourceItem = PhCreateEMenuItem(0, SM_COMMAND_ID_OPEN_SOURCE, L"Open &Task Scheduler", NULL, NULL), ULONG_MAX);
            else if (hasSelection && (items[0]->Source == SmSourceStartupFolderUser || items[0]->Source == SmSourceStartupFolderCommon))
                PhInsertEMenuItem(menu, openSourceItem = PhCreateEMenuItem(0, SM_COMMAND_ID_OPEN_SOURCE, L"Open startup f&older", NULL, NULL), ULONG_MAX);
            else
                PhInsertEMenuItem(menu, openSourceItem = PhCreateEMenuItem(0, SM_COMMAND_ID_OPEN_SOURCE, L"Open &registry key", NULL, NULL), ULONG_MAX);

            PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
            PhInsertEMenuItem(menu, deleteItem = PhCreateEMenuItem(0, SM_COMMAND_ID_DELETE, L"De&lete\bDel", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, SM_COMMAND_ID_COPY, L"&Copy\bCtrl+C", NULL, NULL), ULONG_MAX);
            PhInsertEMenuItem(menu, PhCreateEMenuItem(0, SM_COMMAND_ID_REFRESH, L"&Refresh\bF5", NULL, NULL), ULONG_MAX);
            PhInsertCopyCellEMenuItem(menu, SM_COMMAND_ID_COPY, WindowHandle, contextMenuEvent->Column);
            PhSetFlagsEMenuItem(menu, SM_COMMAND_ID_OPEN_FILE_LOCATION, PH_EMENU_DEFAULT, PH_EMENU_DEFAULT);

            if (!hasSelection)
            {
                PhSetDisabledEMenuItem(enableItem);
                PhSetDisabledEMenuItem(disableItem);
                PhSetDisabledEMenuItem(openFileItem);
                PhSetDisabledEMenuItem(openSourceItem);
                PhSetDisabledEMenuItem(deleteItem);
            }
            else
            {
                if (!anyToggleable || !anyDisabled)
                    PhSetDisabledEMenuItem(enableItem);
                if (!anyToggleable || !anyEnabled)
                    PhSetDisabledEMenuItem(disableItem);
                if (numberOfItems != 1)
                {
                    PhSetDisabledEMenuItem(openFileItem);
                    PhSetDisabledEMenuItem(openSourceItem);
                }
                if (PhIsNullOrEmptyString(items[0]->FileName))
                    PhSetDisabledEMenuItem(openFileItem);
                if (numberOfItems == 1 && items[0]->Source == SmSourcePackagedApp)
                    PhSetDisabledEMenuItem(deleteItem);
            }

            item = PhShowEMenu(
                menu,
                WindowHandle,
                PH_EMENU_SHOW_LEFTRIGHT,
                PH_ALIGN_LEFT | PH_ALIGN_TOP,
                contextMenuEvent->Location.x,
                contextMenuEvent->Location.y
                );

            if (item)
            {
                if (!PhHandleCopyCellEMenuItem(item))
                {
                    SmHandleCommand(WindowHandle, item->Id);
                }
            }

            PhDestroyEMenu(menu);

            if (hasSelection)
                PhFree(items);
        }
        return TRUE;
    }

    return FALSE;
}

// Commands

static VOID SmSetSelectedItemsEnabled(
    _In_ HWND TreeWindowHandle,
    _In_ BOOLEAN Enable
    )
{
    PSM_STARTUP_ITEM* items;
    ULONG numberOfItems;
    NTSTATUS lastStatus = STATUS_SUCCESS;
    BOOLEAN changed = FALSE;

    if (!SmGetSelectedItems(&items, &numberOfItems))
        return;

    for (ULONG i = 0; i < numberOfItems; i++)
    {
        NTSTATUS status;

        if (!items[i]->CanToggle || items[i]->Enabled == Enable)
            continue;

        status = SmSetStartupItemEnabled(items[i], Enable);

        if (NT_SUCCESS(status))
            changed = TRUE;
        else
            lastStatus = status;
    }

    PhFree(items);

    if (!NT_SUCCESS(lastStatus))
    {
        PhShowStatus(
            TreeWindowHandle,
            Enable ? L"Unable to enable the startup entry." : L"Unable to disable the startup entry.",
            lastStatus,
            0
            );
    }

    if (changed)
        SmRefreshStartupList();
}

static VOID SmDeleteSelectedItems(
    _In_ HWND TreeWindowHandle
    )
{
    PSM_STARTUP_ITEM* items;
    ULONG numberOfItems;
    NTSTATUS lastStatus = STATUS_SUCCESS;
    BOOLEAN changed = FALSE;
    PPH_STRING message;
    LONG result;

    if (!SmGetSelectedItems(&items, &numberOfItems))
        return;

    if (numberOfItems == 1)
    {
        message = PhFormatString(
            L"This will permanently remove \"%s\" from %s. The program itself is not uninstalled.",
            PhGetStringOrEmpty(items[0]->Name),
            PhGetStringOrEmpty(items[0]->Location)
            );
    }
    else
    {
        message = PhFormatString(
            L"This will permanently remove %lu startup entries. The programs themselves are not uninstalled.",
            numberOfItems
            );
    }

    result = PhShowMessage2(
        TreeWindowHandle,
        TD_YES_BUTTON | TD_NO_BUTTON,
        TD_WARNING_ICON,
        L"Delete the selected startup entries?",
        L"%s",
        message->Buffer
        );

    PhDereferenceObject(message);

    if (result == IDYES)
    {
        for (ULONG i = 0; i < numberOfItems; i++)
        {
            NTSTATUS status;

            if (items[i]->Source == SmSourcePackagedApp)
                continue; // read-only: uninstall or manage the app from Windows Settings

            status = SmDeleteStartupItem(items[i]);

            if (NT_SUCCESS(status))
                changed = TRUE;
            else
                lastStatus = status;
        }

        if (!NT_SUCCESS(lastStatus))
        {
            PhShowStatus(TreeWindowHandle, L"Unable to delete the startup entry.", lastStatus, 0);
        }
    }

    PhFree(items);

    if (changed)
        SmRefreshStartupList();
}

static VOID SmHandleCommand(
    _In_ HWND TreeWindowHandle,
    _In_ ULONG Id
    )
{
    switch (Id)
    {
    case SM_COMMAND_ID_ENABLE:
        SmSetSelectedItemsEnabled(TreeWindowHandle, TRUE);
        break;
    case SM_COMMAND_ID_DISABLE:
        SmSetSelectedItemsEnabled(TreeWindowHandle, FALSE);
        break;
    case SM_COMMAND_ID_OPEN_FILE_LOCATION:
        {
            PSM_STARTUP_ITEM item;

            if ((item = SmGetSelectedItem()) && !PhIsNullOrEmptyString(item->FileName))
            {
                PhShellExploreFile(TreeWindowHandle, item->FileName->Buffer);
            }
        }
        break;
    case SM_COMMAND_ID_OPEN_SOURCE:
        {
            PSM_STARTUP_ITEM item;

            if (!(item = SmGetSelectedItem()))
                break;

            switch (item->Source)
            {
            case SmSourceRegistryHklm:
            case SmSourceRegistryHkcu:
                PhShellOpenKey2(TreeWindowHandle, item->Location);
                break;
            case SmSourceStartupFolderUser:
            case SmSourceStartupFolderCommon:
                PhShellExecute(TreeWindowHandle, item->Location->Buffer, NULL);
                break;
            case SmSourcePackagedApp:
                PhShellOpenKey2(TreeWindowHandle, item->Location);
                break;
            case SmSourceScheduledTask:
                PhShellExecute(TreeWindowHandle, L"taskschd.msc", NULL);
                break;
            }
        }
        break;
    case SM_COMMAND_ID_DELETE:
        SmDeleteSelectedItems(TreeWindowHandle);
        break;
    case SM_COMMAND_ID_COPY:
        {
            PPH_STRING text;

            text = PhGetTreeNewText(TreeWindowHandle, 0);
            PhSetClipboardString(TreeWindowHandle, &text->sr);
            PhDereferenceObject(text);
        }
        break;
    case SM_COMMAND_ID_REFRESH:
        SmRefreshStartupList();
        break;
    }
}

// Tree list setup

static VOID SmInitializeTreeList(
    _In_ HWND TreeNewHandle
    )
{
    SmNodeList = PhCreateList(64);

    PhSetControlTheme(TreeNewHandle, L"explorer");
    TreeNew_SetRedraw(TreeNewHandle, FALSE);
    TreeNew_SetCallback(TreeNewHandle, SmTreeNewCallback, NULL);

    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_NAME, TRUE, L"Name", 180, PH_ALIGN_LEFT, SM_COLUMN_NAME, 0);
    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_STATUS, TRUE, L"Status", 70, PH_ALIGN_LEFT, SM_COLUMN_STATUS, 0);
    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_TYPE, TRUE, L"Type", 160, PH_ALIGN_LEFT, SM_COLUMN_TYPE, 0);
    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_PUBLISHER, TRUE, L"Publisher", 140, PH_ALIGN_LEFT, SM_COLUMN_PUBLISHER, 0);
    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_COMMAND, TRUE, L"Command", 300, PH_ALIGN_LEFT, SM_COLUMN_COMMAND, DT_PATH_ELLIPSIS);
    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_LOCATION, TRUE, L"Location", 240, PH_ALIGN_LEFT, SM_COLUMN_LOCATION, DT_PATH_ELLIPSIS);
    PhAddTreeNewColumn(TreeNewHandle, SM_COLUMN_IMPACT, TRUE, L"Impact (est.)", 90, PH_ALIGN_LEFT, SM_COLUMN_IMPACT, 0);

    if (PhGetIntegerSetting(L"TreeListCustomRowSize"))
    {
        ULONG treelistCustomRowSize = PhGetIntegerSetting(L"TreeListCustomRowSize");

        if (treelistCustomRowSize < 15)
            treelistCustomRowSize = 15;

        TreeNew_SetRowHeight(TreeNewHandle, treelistCustomRowSize);
    }

    TreeNew_SetEmptyText(TreeNewHandle, &SmEmptyText, 0);
    TreeNew_SetSort(TreeNewHandle, SM_COLUMN_NAME, AscendingSortOrder);
    TreeNew_SetTriState(TreeNewHandle, TRUE);
    TreeNew_SetRedraw(TreeNewHandle, TRUE);

    SmLoadSettingsTreeList(TreeNewHandle);
}

// Tab page

_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
static BOOLEAN NTAPI SmTabPageCallback(
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

            SmTreeNewHandle = PhCreateWindow(
                PH_TREENEW_CLASSNAME,
                NULL,
                WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | TN_STYLE_DOUBLE_BUFFERED | thinRows | treelistBorder | treelistCustomColors,
                clientRect.left,
                clientRect.top,
                clientRect.right - clientRect.left,
                clientRect.bottom - clientRect.top,
                Parameter2,
                NULL,
                NULL,
                &treelistCreateParams
                );

            if (!SmTreeNewHandle)
                return FALSE;

            SmTreeNewCreated = TRUE;

            if (PhGetIntegerSetting(L"EnableThemeSupport"))
            {
                PhInitializeWindowTheme(SmTreeNewHandle, TRUE);
                TreeNew_ThemeSupport(SmTreeNewHandle, TRUE);
            }

            SmInitializeTreeList(SmTreeNewHandle);

            if (SmRefreshPending)
                SmRefreshStartupList();

            if (Parameter1)
            {
                *(HWND*)Parameter1 = SmTreeNewHandle;
            }
        }
        return TRUE;
    case MainTabPageLoadSettings:
        {
            NOTHING;
        }
        return TRUE;
    case MainTabPageSaveSettings:
        {
            SmSaveSettingsTreeList(SmTreeNewHandle);
        }
        return TRUE;
    case MainTabPageSelected:
        {
            BOOLEAN selected = (BOOLEAN)PtrToUlong(Parameter1);

            if (selected)
            {
                SmRefreshStartupList();
            }
        }
        break;
    case MainTabPageExportContent:
        {
            PPH_MAIN_TAB_PAGE_EXPORT_CONTENT exportContent = Parameter1;
            PPH_LIST lines;

            if (!exportContent || !SmTreeNewHandle)
                return FALSE;

            lines = PhGetGenericTreeNewLines(SmTreeNewHandle, exportContent->Mode);

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
            if (SmTreeNewHandle)
                SendMessage(SmTreeNewHandle, WM_SETFONT, (WPARAM)Parameter1, TRUE);
        }
        break;
    case MainTabPageDpiChanged:
        {
            NOTHING;
        }
        break;
    }

    return FALSE;
}

VOID SmInitializeStartupTab(
    VOID
    )
{
    PH_MAIN_TAB_PAGE page;

    memset(&page, 0, sizeof(PH_MAIN_TAB_PAGE));
    page.Name = SmTabPageName;
    page.Callback = SmTabPageCallback;
    SmAddedTabPage = PhPluginCreateTabPage(&page);
}
