/*
 * TaskManagerPlus: customizable keyboard shortcuts.
 *
 * The table, parsing, conflict rules and persistence are in shortcutcore.c (pure C, unit tested). This file is the glue:
 * it owns the live table, builds the main window accelerator table from it (the message loop asks for it on every
 * message, so a change applies at once), exports PhShortcutMatches for plugins (the QuickActions Ctrl+K hook) and
 * implements Options > Shortcuts.
 *
 * Not customizable: Esc (close when "hide on close" is on), the Pause key (always toggles updates), the ToolStatus search
 * key and the tab accelerators ToolStatus owns. Menu items keep showing their default key text.
 */

#include <phapp.h>
#include <settings.h>
#include <phsettings.h>
#include <mainwnd.h>
#include <appsup.h>
#include "shortcutcore.h"

static SHC_ENTRY PhpShortcuts[ShcCount];
static BOOLEAN PhpShortcutsReady = FALSE;
static HACCEL PhpShortcutsAcceleratorTable = NULL;
static UINT PhpShortcutsDispatchMessage = 0;
static PPH_MESSAGE_LOOP_FILTER_ENTRY PhpShortcutsFilter = NULL;

static CONST WORD PhpShortcutCommands[ShcCount] =
{
    ID_HACKER_FINDHANDLESORDLLS, // ShcFind
    ID_HACKER_RUN,               // ShcRun
    ID_HACKER_RUNAS,             // ShcRunAs
    ID_HACKER_SAVE,              // ShcSave
    ID_HELP_LOG,                 // ShcLog
    ID_TAB_NEXT,                 // ShcTabNext
    ID_TAB_PREV,                 // ShcTabPrev
    ID_VIEW_REFRESH,             // ShcRefresh
    ID_VIEW_SYSTEMINFORMATION,   // ShcSystemInfo
    ID_VIEW_UPDATEAUTOMATICALLY, // ShcPause
    0                            // ShcQuickActions: handled by the QuickActions plugin through PhShortcutMatches
};

static HACCEL PhpShortcutsBuildTable(
    VOID
    )
{
    ACCEL accel[ShcCount + 2];
    ULONG count = 0;
    ULONG i;

    // fixed entries
    accel[count].fVirt = FVIRTKEY | FNOINVERT;
    accel[count].key = VK_ESCAPE;
    accel[count].cmd = ID_ESC_EXIT;
    count++;
    accel[count].fVirt = FVIRTKEY | FNOINVERT;
    accel[count].key = VK_PAUSE;
    accel[count].cmd = ID_VIEW_UPDATEAUTOMATICALLY;
    count++;

    for (i = 0; i < ShcCount; i++)
    {
        if (!PhpShortcutCommands[i] || !PhpShortcuts[i].Current.Vk)
            continue;

        accel[count].fVirt = (BYTE)(FVIRTKEY | FNOINVERT);

        if (PhpShortcuts[i].Current.Mods & SHC_MOD_CTRL)
            accel[count].fVirt |= FCONTROL;
        if (PhpShortcuts[i].Current.Mods & SHC_MOD_SHIFT)
            accel[count].fVirt |= FSHIFT;
        if (PhpShortcuts[i].Current.Mods & SHC_MOD_ALT)
            accel[count].fVirt |= FALT;

        accel[count].key = (WORD)PhpShortcuts[i].Current.Vk;
        accel[count].cmd = PhpShortcutCommands[i];
        count++;
    }

    return CreateAcceleratorTable(accel, (int)count);
}

_Function_class_(PH_MESSAGE_LOOP_FILTER)
static BOOLEAN NTAPI PhpShortcutsMessageFilter(
    _In_ PMSG Message,
    _In_ PVOID Context
    )
{
    if (PhpShortcutsDispatchMessage && Message->message == PhpShortcutsDispatchMessage)
    {
        // Test hook: wParam = SHC_MOD_* bits, lParam = virtual key; runs the command bound to it as the accelerator would.
        int index = ShcLookup(PhpShortcuts, ShcCount, (unsigned)Message->wParam, (unsigned)Message->lParam);

        if (index >= 0 && PhpShortcutCommands[index])
            PostMessage(PhMainWndHandle, WM_COMMAND, MAKEWPARAM(PhpShortcutCommands[index], 0), 0);

        return TRUE;
    }

    return FALSE;
}

VOID PhShortcutsInitialize(
    VOID
    )
{
    PPH_STRING text;

    if (PhpShortcutsReady)
        return;

    ShcInitDefaults(PhpShortcuts);
    text = PhGetStringSetting(SETTING_SHORTCUTS);
    ShcDeserialize(PhpShortcuts, ShcCount, text->Buffer);
    PhDereferenceObject(text);
    PhpShortcutsAcceleratorTable = PhpShortcutsBuildTable();
    PhpShortcutsDispatchMessage = RegisterWindowMessageW(L"TaskManagerPlus.Shortcuts.Dispatch");
    PhpShortcutsFilter = PhRegisterMessageLoopFilter(PhpShortcutsMessageFilter, NULL);
    PhpShortcutsReady = TRUE;
}

HACCEL PhShortcutsGetAcceleratorTable(
    VOID
    )
{
    PhShortcutsInitialize();

    if (PhpShortcutsAcceleratorTable)
        return PhpShortcutsAcceleratorTable;

    return LoadAccelerators(NtCurrentImageBase(), MAKEINTRESOURCE(IDR_MAINWND_ACCEL)); // the built-in defaults
}

BOOLEAN NTAPI PhShortcutMatches(
    _In_ PCSTR Id,
    _In_ ULONG VirtualKey
    )
{
    ULONG mods = 0;
    ULONG i;

    PhShortcutsInitialize();

    for (i = 0; i < ShcCount; i++)
    {
        if (!strcmp(PhpShortcuts[i].Id, Id))
            break;
    }

    if (i >= ShcCount || !PhpShortcuts[i].Current.Vk || PhpShortcuts[i].Current.Vk != VirtualKey)
        return FALSE;

    if (GetKeyState(VK_CONTROL) < 0)
        mods |= SHC_MOD_CTRL;
    if (GetKeyState(VK_SHIFT) < 0)
        mods |= SHC_MOD_SHIFT;
    if (GetKeyState(VK_MENU) < 0)
        mods |= SHC_MOD_ALT;

    return mods == PhpShortcuts[i].Current.Mods;
}

static VOID PhpShortcutsSave(
    VOID
    )
{
    wchar_t text[512];
    HACCEL old;
    HACCEL table;

    if (ShcSerialize(PhpShortcuts, ShcCount, text, RTL_NUMBER_OF(text)) != (size_t)-1)
        PhSetStringSetting(SETTING_SHORTCUTS, text);

    table = PhpShortcutsBuildTable();

    if (table)
    {
        old = PhpShortcutsAcceleratorTable;
        PhpShortcutsAcceleratorTable = table;

        if (old)
            DestroyAcceleratorTable(old);
    }
}

// Options > Shortcuts

static HWND PhpShortcutsList = NULL;

static VOID PhpShortcutsRefreshRow(
    _In_ ULONG Index
    )
{
    wchar_t text[48];

    ShcFormat(PhpShortcuts[Index].Current, text, RTL_NUMBER_OF(text));
    PhSetListViewSubItem(PhpShortcutsList, Index, 1, text);
}

static VOID PhpShortcutsSetStatus(
    _In_ HWND WindowHandle,
    _In_ PCWSTR Format,
    ...
    )
{
    va_list argptr;
    PPH_STRING text;

    va_start(argptr, Format);
    text = PhFormatString_V(Format, argptr);
    va_end(argptr);
    PhSetWindowText(GetDlgItem(WindowHandle, IDC_SHORTCUTSTATUS), text->Buffer);
    PhDereferenceObject(text);
}

static INT PhpShortcutsSelected(
    VOID
    )
{
    return ListView_GetNextItem(PhpShortcutsList, -1, LVNI_SELECTED);
}

/** Gives entry Index the combo when allowed; otherwise explains why in the status line. */
static BOOLEAN PhpShortcutsAssign(
    _In_ HWND WindowHandle,
    _In_ ULONG Index,
    _In_ SHC_COMBO Combo
    )
{
    wchar_t name[48];
    int other;

    ShcFormat(Combo, name, RTL_NUMBER_OF(name));

    switch (ShcCheckAssign(PhpShortcuts, ShcCount, Index, Combo, &other))
    {
    case ShcNeedsModifier:
        PhpShortcutsSetStatus(WindowHandle, L"%s would interfere with typing. Add Ctrl or Alt (only F-keys and Pause can stand alone).", name);
        return FALSE;
    case ShcReserved:
        PhpShortcutsSetStatus(WindowHandle, L"%s is reserved by Windows or by text editing and cannot be used.", name);
        return FALSE;
    case ShcConflict:
        PhpShortcutsSetStatus(WindowHandle, L"%s is already used by \"%s\". Unbind or change that one first.", name, PhpShortcuts[other].Label);
        return FALSE;
    }

    PhpShortcuts[Index].Current = Combo;
    PhpShortcutsSave();
    PhpShortcutsRefreshRow(Index);
    PhSetWindowText(GetDlgItem(WindowHandle, IDC_SHORTCUTEDIT), name);
    PhpShortcutsSetStatus(WindowHandle, L"\"%s\" is now %s. This takes effect immediately.", PhpShortcuts[Index].Label, name);
    return TRUE;
}

INT_PTR CALLBACK PhOptionsShortcutsDlgProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        {
            ULONG i;

            PhShortcutsInitialize();
            PhpShortcutsList = GetDlgItem(hwndDlg, IDC_SHORTCUTLIST);
            PhSetListViewStyle(PhpShortcutsList, FALSE, TRUE);
            PhSetControlTheme(PhpShortcutsList, L"explorer");
            PhAddListViewColumn(PhpShortcutsList, 0, 0, 0, LVCFMT_LEFT, 150, L"Action");
            PhAddListViewColumn(PhpShortcutsList, 1, 1, 1, LVCFMT_LEFT, 90, L"Shortcut");
            PhAddListViewColumn(PhpShortcutsList, 2, 2, 2, LVCFMT_LEFT, 90, L"Default");
            ListView_SetExtendedListViewStyleEx(PhpShortcutsList, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);

            for (i = 0; i < ShcCount; i++)
            {
                wchar_t text[48];

                PhAddListViewItem(PhpShortcutsList, MAXINT, (PWSTR)PhpShortcuts[i].Label, NULL);
                ShcFormat(PhpShortcuts[i].Current, text, RTL_NUMBER_OF(text));
                PhSetListViewSubItem(PhpShortcutsList, i, 1, text);
                ShcFormat(PhpShortcuts[i].Default, text, RTL_NUMBER_OF(text));
                PhSetListViewSubItem(PhpShortcutsList, i, 2, text);
            }

            ListView_SetItemState(PhpShortcutsList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (header->hwndFrom == PhpShortcutsList && header->code == LVN_ITEMCHANGED)
            {
                LPNMLISTVIEW view = (LPNMLISTVIEW)lParam;

                if ((view->uChanged & LVIF_STATE) && (view->uNewState & LVIS_SELECTED) && view->iItem >= 0 && view->iItem < ShcCount)
                {
                    wchar_t text[48];

                    ShcFormat(PhpShortcuts[view->iItem].Current, text, RTL_NUMBER_OF(text));
                    PhSetWindowText(GetDlgItem(hwndDlg, IDC_SHORTCUTEDIT), text);
                    PhSetWindowText(GetDlgItem(hwndDlg, IDC_SHORTCUTSTATUS), L"");
                }
            }
        }
        break;
    case WM_COMMAND:
        {
            INT index = PhpShortcutsSelected();

            switch (GET_WM_COMMAND_ID(wParam, lParam))
            {
            case IDC_SHORTCUTSET:
                {
                    PPH_STRING text = PH_AUTO(PhGetWindowText(GetDlgItem(hwndDlg, IDC_SHORTCUTEDIT)));
                    SHC_COMBO combo;

                    if (index < 0)
                    {
                        PhpShortcutsSetStatus(hwndDlg, L"Select an action first.");
                    }
                    else if (!text || !ShcParse(text->Buffer, &combo))
                    {
                        PhpShortcutsSetStatus(hwndDlg, L"That is not a shortcut. Examples: Ctrl+Shift+K, Alt+M, F5, Ctrl+Tab.");
                    }
                    else
                    {
                        PhpShortcutsAssign(hwndDlg, (ULONG)index, combo);
                    }
                }
                break;
            case IDC_SHORTCUTCLEAR:
                if (index >= 0)
                {
                    SHC_COMBO none = { 0, 0 };

                    PhpShortcutsAssign(hwndDlg, (ULONG)index, none);
                }
                break;
            case IDC_SHORTCUTRESET:
                if (index >= 0)
                    PhpShortcutsAssign(hwndDlg, (ULONG)index, PhpShortcuts[index].Default);
                break;
            case IDC_SHORTCUTRESETALL:
                {
                    ULONG i;

                    for (i = 0; i < ShcCount; i++)
                    {
                        PhpShortcuts[i].Current = PhpShortcuts[i].Default;
                        PhpShortcutsRefreshRow(i);
                    }

                    PhpShortcutsSave();

                    if (index >= 0)
                    {
                        wchar_t text[48];

                        ShcFormat(PhpShortcuts[index].Current, text, RTL_NUMBER_OF(text));
                        PhSetWindowText(GetDlgItem(hwndDlg, IDC_SHORTCUTEDIT), text);
                    }

                    PhpShortcutsSetStatus(hwndDlg, L"All shortcuts are back to their defaults.");
                }
                break;
            }
        }
        break;
    case WM_DESTROY:
        PhpShortcutsList = NULL;
        break;
    }

    return FALSE;
}
