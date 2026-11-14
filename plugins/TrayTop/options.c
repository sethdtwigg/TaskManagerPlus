/*
 * TaskManagerPlus TrayTop: Options > Tray top processes (on/off and how many processes per list).
 */

#include "traytop.h"

INT_PTR CALLBACK TtOptionsDlgProc(
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
            Button_SetCheck(GetDlgItem(hwndDlg, IDC_TT_ENABLED), PhGetIntegerSetting(SETTING_NAME_ENABLED) ? BST_CHECKED : BST_UNCHECKED);
            PhSetDialogItemValue(hwndDlg, IDC_TT_COUNT, TtClampCount((LONG)PhGetIntegerSetting(SETTING_NAME_COUNT)), FALSE);
            SendMessage(GetDlgItem(hwndDlg, IDC_TT_SPIN), UDM_SETRANGE32, TT_COUNT_MIN, TT_COUNT_MAX);
        }
        break;
    case WM_DESTROY:
        {
            LONG count = (LONG)PhGetDialogItemValue(hwndDlg, IDC_TT_COUNT);

            PhSetIntegerSetting(SETTING_NAME_ENABLED, Button_GetCheck(GetDlgItem(hwndDlg, IDC_TT_ENABLED)) == BST_CHECKED);
            PhSetIntegerSetting(SETTING_NAME_COUNT, TtClampCount(count)); // out-of-range or empty input is clamped, never stored raw
        }
        break;
    }

    return FALSE;
}
