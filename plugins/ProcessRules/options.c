/*
 * TaskManagerPlus ProcessRules plugin - options section
 *
 * Values are written back in WM_DESTROY, which is how every other System
 * Informer options page applies its settings (the options window destroys
 * the section for both OK and Cancel).
 */

#include "processrules.h"

INT_PTR CALLBACK TmpOptionsDlgProc(
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
            PPH_STRING dumpDirectory;
            PPH_STRING rulesFileName;
            PPH_STRING rulesFileText;

            Button_SetCheck(GetDlgItem(WindowHandle, IDC_ENGINE), PhGetIntegerSetting(SETTING_NAME_ENGINE_ENABLED) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_NOTIFY), PhGetIntegerSetting(SETTING_NAME_NOTIFY) ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(GetDlgItem(WindowHandle, IDC_LOG), PhGetIntegerSetting(SETTING_NAME_LOG) ? BST_CHECKED : BST_UNCHECKED);
            PhSetDialogItemValue(WindowHandle, IDC_GUARD_GLOBAL, PhGetIntegerSetting(SETTING_NAME_GUARD_GLOBAL), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_GUARD_RULE, PhGetIntegerSetting(SETTING_NAME_GUARD_RULE), FALSE);
            PhSetDialogItemValue(WindowHandle, IDC_DUMPS, PhGetIntegerSetting(SETTING_NAME_DUMPS_PER_HOUR), FALSE);

            dumpDirectory = PhGetStringSetting(SETTING_NAME_DUMP_DIRECTORY);
            PhSetDialogItemText(WindowHandle, IDC_DUMPDIR, PhGetStringOrEmpty(dumpDirectory));
            PhDereferenceObject(dumpDirectory);

            if (rulesFileName = TmpGetRulesFileName())
            {
                rulesFileText = PhConcatStrings2(L"Rules are stored in: ", PhGetString(rulesFileName));
                PhSetDialogItemText(WindowHandle, IDC_RULESFILE, PhGetString(rulesFileText));
                PhDereferenceObject(rulesFileText);
                PhDereferenceObject(rulesFileName);
            }
        }
        break;
    case WM_DESTROY:
        {
            PPH_STRING dumpDirectory;

            PhSetIntegerSetting(SETTING_NAME_ENGINE_ENABLED, Button_GetCheck(GetDlgItem(WindowHandle, IDC_ENGINE)) == BST_CHECKED);
            PhSetIntegerSetting(SETTING_NAME_NOTIFY, Button_GetCheck(GetDlgItem(WindowHandle, IDC_NOTIFY)) == BST_CHECKED);
            PhSetIntegerSetting(SETTING_NAME_LOG, Button_GetCheck(GetDlgItem(WindowHandle, IDC_LOG)) == BST_CHECKED);
            PhSetIntegerSetting(SETTING_NAME_GUARD_GLOBAL, (ULONG)PhGetDialogItemValue(WindowHandle, IDC_GUARD_GLOBAL));
            PhSetIntegerSetting(SETTING_NAME_GUARD_RULE, (ULONG)PhGetDialogItemValue(WindowHandle, IDC_GUARD_RULE));
            PhSetIntegerSetting(SETTING_NAME_DUMPS_PER_HOUR, (ULONG)PhGetDialogItemValue(WindowHandle, IDC_DUMPS));

            dumpDirectory = PhGetWindowText(GetDlgItem(WindowHandle, IDC_DUMPDIR));
            PhSetStringSetting2(SETTING_NAME_DUMP_DIRECTORY, &dumpDirectory->sr);
            PhDereferenceObject(dumpDirectory);
        }
        break;
    case WM_CTLCOLORBTN:
        return HANDLE_WM_CTLCOLORBTN(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORDLG:
        return HANDLE_WM_CTLCOLORDLG(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    case WM_CTLCOLORSTATIC:
        return HANDLE_WM_CTLCOLORSTATIC(WindowHandle, wParam, lParam, PhWindowThemeControlColor);
    }

    return FALSE;
}
