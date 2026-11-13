/*
 * TaskManagerPlus Profiles: the "save current settings as profile" and "manage profiles" dialogs.
 */

#include "profiles.h"

static VOID PrThemeDialog(
    _In_ HWND WindowHandle
    )
{
    PhInitializeWindowTheme(WindowHandle, !!PhGetIntegerSetting(L"EnableThemeSupport"));
}

// ---------------------------------------------------------------- save

static INT_PTR CALLBACK PrSaveDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        {
            PhCenterWindow(WindowHandle, GetParent(WindowHandle));
            PrThemeDialog(WindowHandle);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_CAP_ENABLED), BST_CHECKED);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_CAP_PRIORITY), BST_CHECKED);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_CAP_THEME), BST_CHECKED);
            Edit_LimitText(GetDlgItem(WindowHandle, IDC_PROFILE_NAME), PF_NAME_CHARS - 1);

            if (PrStore.Active[0])
                PhSetDialogItemText(WindowHandle, IDC_PROFILE_NAME, PrStore.Active);

            SetFocus(GetDlgItem(WindowHandle, IDC_PROFILE_NAME));
            return FALSE;
        }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDCANCEL:
            EndDialog(WindowHandle, IDCANCEL);
            break;
        case IDOK:
            {
                PPH_STRING name = PhGetWindowText(GetDlgItem(WindowHandle, IDC_PROFILE_NAME));
                unsigned flags = 0;
                BOOLEAN replace;

                if (Button_GetCheck(GetDlgItem(WindowHandle, IDC_CAP_ENABLED)) == BST_CHECKED)
                    flags |= PF_CAPTURE_ENABLED;
                if (Button_GetCheck(GetDlgItem(WindowHandle, IDC_CAP_PRIORITY)) == BST_CHECKED)
                    flags |= PF_CAPTURE_PRIORITY;
                if (Button_GetCheck(GetDlgItem(WindowHandle, IDC_CAP_THEME)) == BST_CHECKED)
                    flags |= PF_CAPTURE_THEME;

                if (!PfValidName(name->Buffer))
                {
                    PhShowError2(WindowHandle, L"Profile name", L"%s", L"Enter a name of 1 to 47 characters.");
                    PhDereferenceObject(name);
                    break;
                }

                if (!flags)
                {
                    PhShowError2(WindowHandle, L"Profile", L"%s", L"Tick at least one thing to include.");
                    PhDereferenceObject(name);
                    break;
                }

                replace = PfStoreFind(&PrStore, name->Buffer) >= 0;

                if (replace && PhShowMessage2(WindowHandle, TD_YES_BUTTON | TD_NO_BUTTON, TD_WARNING_ICON, L"Replace profile?", L"A profile named \"%s\" exists. Replace it with the current settings?", name->Buffer) != IDYES)
                {
                    PhDereferenceObject(name);
                    break;
                }

                if (PrSaveCurrentAs(name->Buffer, flags))
                    EndDialog(WindowHandle, IDOK);
                else
                    PhShowError2(WindowHandle, L"Profile", L"%s", L"The profile could not be saved (too many profiles?).");

                PhDereferenceObject(name);
            }
            break;
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLOREDIT:
        return HANDLE_WM_CTLCOLOREDIT(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

VOID PrShowSaveDialog(
    _In_ HWND OwnerWindow
    )
{
    PhDialogBox(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_SAVE_PROFILE), OwnerWindow, PrSaveDlgProc, NULL);
}

// ---------------------------------------------------------------- manage

static VOID PrFillList(
    _In_ HWND WindowHandle,
    _In_opt_ PCWSTR Select
    )
{
    HWND list = GetDlgItem(WindowHandle, IDC_PROFILE_LIST);
    ULONG i;
    INT selected = -1;

    ListBox_ResetContent(list);

    for (i = 0; i < PrStore.Count; i++)
    {
        PPH_STRING text;

        if (!_wcsicmp(PrStore.Active, PrStore.Profiles[i]->Name))
            text = PhFormatString(L"%s   (active)", PrStore.Profiles[i]->Name);
        else
            text = PhCreateString(PrStore.Profiles[i]->Name);

        ListBox_AddString(list, text->Buffer);
        PhDereferenceObject(text);

        if (Select && !_wcsicmp(Select, PrStore.Profiles[i]->Name))
            selected = (INT)i;
    }

    if (selected < 0 && PrStore.Count)
        selected = 0;

    if (selected >= 0)
        ListBox_SetCurSel(list, selected);
}

static VOID PrUpdateDetails(
    _In_ HWND WindowHandle
    )
{
    HWND list = GetDlgItem(WindowHandle, IDC_PROFILE_LIST);
    INT index = ListBox_GetCurSel(list);
    PPH_STRING text;
    BOOLEAN has = index >= 0 && (ULONG)index < PrStore.Count;

    if (has)
    {
        PF_PROFILE *p = PrStore.Profiles[index];
        ULONG on = 0;
        ULONG off = 0;
        ULONG priorities = 0;
        ULONG i;

        for (i = 0; i < p->RuleCount; i++)
        {
            if (p->Rules[i].EnabledMode == PF_ON)
                on++;
            else if (p->Rules[i].EnabledMode == PF_OFF)
                off++;

            if (p->Rules[i].HasPriority)
                priorities++;
        }

        text = PhFormatString(
            L"Rules switched on: %lu, switched off: %lu, start priorities: %lu\r\nTheme: %hs",
            on, off, priorities,
            p->HasTheme && PfThemeName(p->Theme) ? PfThemeName(p->Theme) : "not changed"
            );
    }
    else
    {
        text = PhCreateString(L"No profiles yet. Use System > Profiles > Save current settings as profile, or Import.");
    }

    PhSetDialogItemText(WindowHandle, IDC_PROFILE_INFO, text->Buffer);
    PhDereferenceObject(text);
    EnableWindow(GetDlgItem(WindowHandle, IDC_APPLY), has);
    EnableWindow(GetDlgItem(WindowHandle, IDC_DELETE), has);
    EnableWindow(GetDlgItem(WindowHandle, IDC_EXPORT), PrStore.Count != 0);
    EnableWindow(GetDlgItem(WindowHandle, IDC_REVERT), PrStore.Previous != NULL);
}

static PPH_STRING PrSelectedName(
    _In_ HWND WindowHandle
    )
{
    INT index = ListBox_GetCurSel(GetDlgItem(WindowHandle, IDC_PROFILE_LIST));

    if (index >= 0 && (ULONG)index < PrStore.Count)
        return PhCreateString(PrStore.Profiles[index]->Name);

    return NULL;
}

static VOID PrDoExport(
    _In_ HWND WindowHandle
    )
{
    static CONST PH_FILETYPE_FILTER filters[] =
    {
        { L"JSON files (*.json)", L"*.json" },
        { L"All files (*.*)", L"*.*" }
    };
    PPH_STRING selected = PrSelectedName(WindowHandle);
    PVOID dialog = PhCreateSaveFileDialog();

    PhSetFileDialogFilter(dialog, (PPH_FILETYPE_FILTER)filters, RTL_NUMBER_OF(filters));
    PhSetFileDialogFileName(dialog, selected ? L"taskmanagerplus-profile.json" : L"taskmanagerplus-profiles.json");

    if (PhShowFileDialog(WindowHandle, dialog))
    {
        PPH_STRING fileName = PhGetFileDialogFileName(dialog);
        NTSTATUS status = PrExportToFile(fileName, selected ? selected->Buffer : NULL);

        if (!NT_SUCCESS(status))
            PhShowStatus(WindowHandle, L"Unable to export", status, 0);

        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(dialog);
    PhClearReference(&selected);
}

static VOID PrDoImport(
    _In_ HWND WindowHandle
    )
{
    static CONST PH_FILETYPE_FILTER filters[] =
    {
        { L"JSON files (*.json)", L"*.json" },
        { L"All files (*.*)", L"*.*" }
    };
    PVOID dialog = PhCreateOpenFileDialog();

    PhSetFileDialogFilter(dialog, (PPH_FILETYPE_FILTER)filters, RTL_NUMBER_OF(filters));

    if (PhShowFileDialog(WindowHandle, dialog))
    {
        PPH_STRING fileName = PhGetFileDialogFileName(dialog);
        ULONG imported;
        ULONG skipped;
        NTSTATUS status = PrImportFromFile(fileName, &imported, &skipped);

        if (NT_SUCCESS(status))
            PhShowInformation2(WindowHandle, L"Import profiles", L"Imported %lu profile(s), skipped %lu invalid or surplus entry(ies).", imported, skipped);
        else
            PhShowStatus(WindowHandle, L"Unable to import", status, 0);

        PhDereferenceObject(fileName);
    }

    PhFreeFileDialog(dialog);
}

static INT_PTR CALLBACK PrManageDlgProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_INITDIALOG:
        PhCenterWindow(WindowHandle, GetParent(WindowHandle));
        PrThemeDialog(WindowHandle);
        PrFillList(WindowHandle, PrStore.Active);
        PrUpdateDetails(WindowHandle);
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_PROFILE_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE)
                PrUpdateDetails(WindowHandle);
            else if (HIWORD(wParam) == LBN_DBLCLK)
                SendMessage(WindowHandle, WM_COMMAND, MAKEWPARAM(IDC_APPLY, BN_CLICKED), 0);
            break;
        case IDC_APPLY:
            {
                PPH_STRING name = PrSelectedName(WindowHandle);

                if (name)
                {
                    PrApplyProfile(name->Buffer);
                    PrFillList(WindowHandle, name->Buffer);
                    PrUpdateDetails(WindowHandle);
                    PhDereferenceObject(name);
                }
            }
            break;
        case IDC_DELETE:
            {
                PPH_STRING name = PrSelectedName(WindowHandle);

                if (name)
                {
                    if (PhShowMessage2(WindowHandle, TD_YES_BUTTON | TD_NO_BUTTON, TD_WARNING_ICON, L"Delete profile?", L"Delete the profile \"%s\"? This does not change any rule.", name->Buffer) == IDYES)
                    {
                        PfStoreRemove(&PrStore, name->Buffer);
                        PrSave();
                        PrFillList(WindowHandle, NULL);
                        PrUpdateDetails(WindowHandle);
                    }

                    PhDereferenceObject(name);
                }
            }
            break;
        case IDC_REVERT:
            if (PrRevert())
            {
                PrFillList(WindowHandle, NULL);
                PrUpdateDetails(WindowHandle);
            }
            break;
        case IDC_EXPORT:
            PrDoExport(WindowHandle);
            break;
        case IDC_IMPORT:
            PrDoImport(WindowHandle);
            PrFillList(WindowHandle, NULL);
            PrUpdateDetails(WindowHandle);
            break;
        case IDCANCEL:
        case IDOK:
            EndDialog(WindowHandle, IDOK);
            break;
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORLISTBOX:
        return HANDLE_WM_CTLCOLORLISTBOX(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLOREDIT:
        return HANDLE_WM_CTLCOLOREDIT(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}

VOID PrShowManageDialog(
    _In_ HWND OwnerWindow
    )
{
    PhDialogBox(PluginInstance->DllBase, MAKEINTRESOURCE(IDD_MANAGE_PROFILES), OwnerWindow, PrManageDlgProc, NULL);
}
