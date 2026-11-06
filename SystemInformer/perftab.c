/*
 * TaskManagerPlus: Performance tab.
 *
 * Hosts the System Information UI (sysinfo.c and its CPU, memory, I/O and plugin sections) in a
 * main-window tab. The tab page window is a plain host window; the sysinfo window is created as
 * its child on the main thread and laid out as a sidebar of tiles (mini graph, name, subtitle)
 * with the selected section on the right. All graph drawing is the existing section code.
 *
 * The section code is single-instance. When the standalone System Information window is open
 * the tab shows a short hint with a button that closes the standalone window and takes over;
 * opening the standalone window while the tab hosts the sections hands the sections back.
 */

#include <phapp.h>
#include <phplug.h>
#include <mainwnd.h>
#include <guisup.h>
#include <sysinfo.h>
#include <sysinfop.h>
#include <mainwndp.h>

#define PERFTAB_CLASS_NAME L"TaskManagerPlusPerformanceTab"
#define PERFTAB_TIMER_ID 1
#define PERFTAB_TIMER_INTERVAL 500
#define PERFTAB_ID_TAKEOVER 100
#define PERFTAB_PADDING 8

PPH_MAIN_TAB_PAGE PhMwpPerformancePage = NULL;

static HWND PerfTabHostHandle = NULL;
static HWND PerfTabSysInfoHandle = NULL;
static HWND PerfTabHintHandle = NULL;
static HWND PerfTabTakeOverHandle = NULL;
static BOOLEAN PerfTabClassRegistered = FALSE;

static COLORREF PhpPerfTabBackColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowBackgroundColor : GetSysColor(COLOR_WINDOW);
}

static COLORREF PhpPerfTabTextColor(
    VOID
    )
{
    return PhEnableThemeSupport ? PhThemeWindowTextColor : GetSysColor(COLOR_WINDOWTEXT);
}

static VOID PhpPerfTabLayout(
    VOID
    )
{
    RECT clientRect;

    if (!PerfTabHostHandle || !GetClientRect(PerfTabHostHandle, &clientRect))
        return;

    if (PerfTabSysInfoHandle)
    {
        SetWindowPos(
            PerfTabSysInfoHandle,
            NULL,
            0,
            0,
            clientRect.right,
            clientRect.bottom,
            SWP_NOACTIVATE | SWP_NOZORDER
            );
    }
    else if (PerfTabHintHandle && PerfTabTakeOverHandle)
    {
        ULONG dpi = PhGetWindowDpi(PerfTabHostHandle);
        LONG padding = PhScaleToDisplay(PERFTAB_PADDING, dpi);
        LONG labelHeight = PhScaleToDisplay(24, dpi);
        LONG buttonHeight = PhScaleToDisplay(32, dpi);
        LONG buttonWidth = PhScaleToDisplay(240, dpi);

        SetWindowPos(
            PerfTabHintHandle,
            NULL,
            padding * 2,
            padding * 2,
            max(clientRect.right - padding * 4, 0),
            labelHeight,
            SWP_NOACTIVATE | SWP_NOZORDER
            );
        SetWindowPos(
            PerfTabTakeOverHandle,
            NULL,
            padding * 2,
            padding * 2 + labelHeight + padding,
            buttonWidth,
            buttonHeight,
            SWP_NOACTIVATE | SWP_NOZORDER
            );
    }
}

static VOID PhpPerfTabShowHint(
    _In_ BOOLEAN Show
    )
{
    if (PerfTabHintHandle)
        ShowWindow(PerfTabHintHandle, Show ? SW_SHOW : SW_HIDE);
    if (PerfTabTakeOverHandle)
        ShowWindow(PerfTabTakeOverHandle, Show ? SW_SHOW : SW_HIDE);
}

/**
 * Makes the tab show the sysinfo sections, or the hint when the standalone window owns them.
 */
static VOID PhpPerfTabEnsure(
    VOID
    )
{
    if (!PerfTabHostHandle || PerfTabSysInfoHandle)
        return;

    if (PhSipIsStandaloneOpen())
    {
        PhpPerfTabShowHint(TRUE);
        PhpPerfTabLayout();
        SetTimer(PerfTabHostHandle, PERFTAB_TIMER_ID, PERFTAB_TIMER_INTERVAL, NULL);
        return;
    }

    PerfTabSysInfoHandle = PhSipCreateEmbeddedWindow(PerfTabHostHandle);

    if (PerfTabSysInfoHandle)
    {
        KillTimer(PerfTabHostHandle, PERFTAB_TIMER_ID);
        PhpPerfTabShowHint(FALSE);
        PhpPerfTabLayout();
        ShowWindow(PerfTabSysInfoHandle, SW_SHOW);
    }
    else
    {
        PhpPerfTabShowHint(TRUE);
        PhpPerfTabLayout();
    }
}

/**
 * Called by sysinfo.c before the standalone window is opened: gives the sections back.
 */
VOID PhPerfTabReleaseSysInfo(
    VOID
    )
{
    if (!PerfTabSysInfoHandle)
        return;

    PhSipDestroyEmbeddedWindow();
    PerfTabSysInfoHandle = NULL;

    if (PerfTabHostHandle)
    {
        PhpPerfTabShowHint(TRUE);
        PhpPerfTabLayout();
        SetTimer(PerfTabHostHandle, PERFTAB_TIMER_ID, PERFTAB_TIMER_INTERVAL, NULL);
    }
}

static LRESULT CALLBACK PhpPerfTabWindowProc(
    _In_ HWND WindowHandle,
    _In_ UINT WindowMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (WindowMessage)
    {
    case WM_SIZE:
        {
            PhpPerfTabLayout();
        }
        return 0;
    case WM_ERASEBKGND:
        {
            RECT clientRect;

            if (GetClientRect(WindowHandle, &clientRect))
            {
                SetDCBrushColor((HDC)wParam, PhpPerfTabBackColor());
                FillRect((HDC)wParam, &clientRect, PhGetStockBrush(DC_BRUSH));
            }
        }
        return TRUE;
    case WM_CTLCOLORSTATIC:
        {
            SetBkMode((HDC)wParam, TRANSPARENT);
            SetTextColor((HDC)wParam, PhpPerfTabTextColor());
            SetDCBrushColor((HDC)wParam, PhpPerfTabBackColor());
        }
        return (LRESULT)PhGetStockBrush(DC_BRUSH);
    case WM_COMMAND:
        {
            if (LOWORD(wParam) == PERFTAB_ID_TAKEOVER && HIWORD(wParam) == BN_CLICKED)
            {
                HWND standaloneWindow = PhSipWindow;

                if (PhSipIsStandaloneOpen() && standaloneWindow)
                    PostMessage(standaloneWindow, WM_CLOSE, 0, 0);
                else
                    PhpPerfTabEnsure();
            }
        }
        return 0;
    case WM_TIMER:
        {
            if (wParam == PERFTAB_TIMER_ID && !PerfTabSysInfoHandle)
            {
                // The standalone window went away: take over when the tab is visible.
                if (!PhSipIsStandaloneOpen() && PhMwpPerformancePage && PhMwpPerformancePage->Selected)
                    PhpPerfTabEnsure();
            }
        }
        return 0;
    }

    return DefWindowProc(WindowHandle, WindowMessage, wParam, lParam);
}

static BOOLEAN PhpPerfTabCreateWindow(
    _In_ HWND ParentWindowHandle
    )
{
    WNDCLASSEX windowClass;
    HFONT font;

    if (!PerfTabClassRegistered)
    {
        memset(&windowClass, 0, sizeof(WNDCLASSEX));
        windowClass.cbSize = sizeof(WNDCLASSEX);
        windowClass.lpfnWndProc = PhpPerfTabWindowProc;
        windowClass.hInstance = PhInstanceHandle;
        windowClass.hCursor = PhLoadCursor(NULL, IDC_ARROW);
        windowClass.lpszClassName = PERFTAB_CLASS_NAME;

        if (RegisterClassEx(&windowClass) == INVALID_ATOM)
            return FALSE;

        PerfTabClassRegistered = TRUE;
    }

    PerfTabHostHandle = PhCreateWindow(
        PERFTAB_CLASS_NAME,
        NULL,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        ParentWindowHandle,
        NULL,
        PhInstanceHandle,
        NULL
        );

    if (!PerfTabHostHandle)
        return FALSE;

    PerfTabHintHandle = PhCreateWindow(
        WC_STATIC,
        L"System Information is open in its own window.",
        WS_CHILD | SS_LEFT | SS_NOPREFIX,
        0, 0, 0, 0,
        PerfTabHostHandle,
        NULL,
        NULL,
        NULL
        );
    PerfTabTakeOverHandle = PhCreateWindow(
        WC_BUTTON,
        L"Close it and show it here",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0,
        PerfTabHostHandle,
        UlongToHandle(PERFTAB_ID_TAKEOVER),
        NULL,
        NULL
        );

    font = PhTreeWindowFont;

    if (font)
    {
        SetWindowFont(PerfTabHintHandle, font, FALSE);
        SetWindowFont(PerfTabTakeOverHandle, font, FALSE);
    }

    if (PhEnableThemeSupport)
        PhInitializeWindowTheme(PerfTabHostHandle, PhEnableThemeSupport);

    return TRUE;
}

/**
 * Performance tab page callback function.
 */
_Function_class_(PH_MAIN_TAB_PAGE_CALLBACK)
BOOLEAN PhMwpPerformancePageCallback(
    _In_ PPH_MAIN_TAB_PAGE Page,
    _In_ PH_MAIN_TAB_PAGE_MESSAGE Message,
    _In_ PVOID Parameter1,
    _In_ PVOID Parameter2
    )
{
    switch (Message)
    {
    case MainTabPageCreate:
        {
            PhMwpPerformancePage = Page;
        }
        break;
    case MainTabPageCreateWindow:
        {
            if (!PerfTabHostHandle && !PhpPerfTabCreateWindow((HWND)Parameter2))
                return FALSE;

            if (Parameter1)
                *(HWND *)Parameter1 = PerfTabHostHandle;
        }
        return TRUE;
    case MainTabPageSelected:
        {
            if (PtrToUlong(Parameter1))
                PhpPerfTabEnsure();
        }
        break;
    case MainTabPageFontChanged:
        {
            HFONT font = (HFONT)Parameter1;

            if (font && PerfTabHintHandle)
            {
                SetWindowFont(PerfTabHintHandle, font, TRUE);
                SetWindowFont(PerfTabTakeOverHandle, font, TRUE);
            }
        }
        break;
    case MainTabPageDpiChanged:
        {
            if (PerfTabSysInfoHandle)
            {
                ULONG dpi = PhGetWindowDpi(PerfTabHostHandle);

                SendMessage(PerfTabSysInfoHandle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), 0);
            }
        }
        break;
    case MainTabPageDestroy:
        {
            if (PerfTabSysInfoHandle)
            {
                PhSipDestroyEmbeddedWindow();
                PerfTabSysInfoHandle = NULL;
            }
        }
        break;
    }

    return FALSE;
}
