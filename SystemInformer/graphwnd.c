/*
 * TaskManagerPlus: detached graph windows.
 *
 * Each mini meter (CPU, Memory, Disk, Network, GPU, VRAM) can be popped out into its own small
 * top-level window that shows the same live history graph as the meter tile. The windows are
 * always on top by default (right-click toggles that), are resizable, repaint from the sample taken
 * by PhMiniMetersUpdate (no timer of their own) and are drawn from the active theme palette, so
 * light, dark and OLED all follow the Options > Themes choice.
 *
 * Persistence: the DetachedGraphs string setting holds one entry per meter that was ever
 * placed: "index,x,y,width,height,topmost,open;" (screen coordinates in pixels). Windows that
 * were open at the last exit are reopened by PhDetachedGraphsInitialization; a saved rectangle
 * that no longer intersects any monitor is moved back onto the nearest one.
 *
 * Data comes from PhMiniMetersGetMeter, so no extra sampling is done. Everything here runs on the
 * main thread.
 */

#include <phapp.h>
#include <phplug.h>
#include <phsettings.h>
#include <settings.h>
#include <guisup.h>
#include <emenu.h>
#include <mainwnd.h>
#include <mainwndp.h>
#include <sysinfop.h>
#include <minimeters.h>
#include <graphwnd.h>

#define GW_CLASS_NAME L"PhDetachedGraph"

// Layout in pixels at 96 dpi.
#define GW_DEFAULT_W 264
#define GW_DEFAULT_H 136
#define GW_MIN_W 136
#define GW_MIN_H 88
#define GW_MARGIN 8
#define GW_HEADER 24

#define GW_COMMAND_TOPMOST 1
#define GW_COMMAND_CLOSE 2

typedef struct _GW_STATE
{
    HWND Window;
    BOOLEAN HasRect;
    BOOLEAN TopMost;
    BOOLEAN Open;
    RECT Rect; // screen coordinates
} GW_STATE;

static GW_STATE GwState[PH_MINIMETER_COUNT];
static BOOLEAN GwClassRegistered = FALSE;
static COLORREF GwLastBackground = CLR_INVALID;

static LONG GwScale(
    _In_ LONG Value,
    _In_ LONG Dpi
    )
{
    return MulDiv(Value, Dpi, 96);
}

static COLORREF GwPanelColor(VOID)
{
    return PhEnableThemeSupport ? PhThemeWindowBackgroundColor : GetSysColor(COLOR_WINDOW);
}

static COLORREF GwTextColor(VOID)
{
    return PhEnableThemeSupport ? PhThemeWindowTextColor : GetSysColor(COLOR_WINDOWTEXT);
}

static COLORREF GwDisabledColor(VOID)
{
    return PhEnableThemeSupport ? PhThemeWindowDisabledTextColor : GetSysColor(COLOR_GRAYTEXT);
}

// Persistence

static VOID GwCaptureRects(VOID)
{
    for (ULONG i = 0; i < PH_MINIMETER_COUNT; i++)
    {
        if (GwState[i].Window && IsWindow(GwState[i].Window) && !IsIconic(GwState[i].Window))
        {
            GetWindowRect(GwState[i].Window, &GwState[i].Rect);
            GwState[i].HasRect = TRUE;
        }
    }
}

static VOID GwSave(VOID)
{
    PH_STRING_BUILDER sb;
    PPH_STRING text;

    GwCaptureRects();
    PhInitializeStringBuilder(&sb, 128);

    for (ULONG i = 0; i < PH_MINIMETER_COUNT; i++)
    {
        PPH_STRING entry;

        if (!GwState[i].HasRect)
            continue;

        entry = PhFormatString(
            L"%lu,%ld,%ld,%ld,%ld,%d,%d;",
            i,
            GwState[i].Rect.left,
            GwState[i].Rect.top,
            GwState[i].Rect.right - GwState[i].Rect.left,
            GwState[i].Rect.bottom - GwState[i].Rect.top,
            GwState[i].TopMost ? 1 : 0,
            GwState[i].Open ? 1 : 0
            );
        PhAppendStringBuilder(&sb, &entry->sr);
        PhDereferenceObject(entry);
    }

    text = PhFinalStringBuilderString(&sb);
    PhSetStringSetting2(SETTING_DETACHED_GRAPHS, &text->sr);
    PhDereferenceObject(text);
}

static VOID GwLoad(VOID)
{
    PPH_STRING setting;
    PCWSTR cursor;

    for (ULONG i = 0; i < PH_MINIMETER_COUNT; i++)
        GwState[i].TopMost = TRUE; // default: always on top

    setting = PhGetStringSetting(SETTING_DETACHED_GRAPHS);
    cursor = setting->Buffer;

    while (*cursor)
    {
        PWSTR end;
        long values[7];
        ULONG count;

        for (count = 0; count < RTL_NUMBER_OF(values); count++)
        {
            values[count] = wcstol(cursor, &end, 10);

            if (end == cursor)
                break;

            cursor = end;

            if (*cursor == L',' || *cursor == L';')
                cursor++;
        }

        if (count == RTL_NUMBER_OF(values) &&
            values[0] >= 0 && values[0] < PH_MINIMETER_COUNT &&
            values[3] > 0 && values[4] > 0)
        {
            GW_STATE *state = &GwState[values[0]];

            state->Rect.left = values[1];
            state->Rect.top = values[2];
            state->Rect.right = values[1] + values[3];
            state->Rect.bottom = values[2] + values[4];
            state->HasRect = TRUE;
            state->TopMost = values[5] != 0;
            state->Open = values[6] != 0;
        }
        else
        {
            // Skip a malformed entry up to the next separator.
            while (*cursor && *cursor != L';')
                cursor++;

            if (*cursor == L';')
                cursor++;
        }
    }

    PhDereferenceObject(setting);
}

/**
 * Moves a saved rectangle onto the nearest monitor when it is (mostly) off screen.
 */
static VOID GwClampToMonitor(
    _Inout_ PRECT Rect
    )
{
    MONITORINFO info = { sizeof(MONITORINFO) };
    HMONITOR monitor = MonitorFromRect(Rect, MONITOR_DEFAULTTONEAREST);
    LONG width = Rect->right - Rect->left;
    LONG height = Rect->bottom - Rect->top;
    RECT visible;

    if (!monitor || !GetMonitorInfo(monitor, &info))
        return;

    if (width > info.rcWork.right - info.rcWork.left)
        width = info.rcWork.right - info.rcWork.left;
    if (height > info.rcWork.bottom - info.rcWork.top)
        height = info.rcWork.bottom - info.rcWork.top;

    // Keep at least a 64 px square of the window on the work area so it can be grabbed.
    if (!IntersectRect(&visible, Rect, &info.rcWork) || visible.right - visible.left < 64 || visible.bottom - visible.top < 64)
    {
        Rect->left = info.rcWork.left + 32;
        Rect->top = info.rcWork.top + 32;
    }

    if (Rect->left + width > info.rcWork.right)
        Rect->left = info.rcWork.right - width;
    if (Rect->top + height > info.rcWork.bottom)
        Rect->top = info.rcWork.bottom - height;
    if (Rect->left < info.rcWork.left)
        Rect->left = info.rcWork.left;
    if (Rect->top < info.rcWork.top)
        Rect->top = info.rcWork.top;

    Rect->right = Rect->left + width;
    Rect->bottom = Rect->top + height;
}

// Painting

static VOID GwDrawGraph(
    _In_ HDC Dc,
    _In_ PVOID Bits,
    _In_ LONG Width,
    _In_ LONG Height,
    _In_ LONG Dpi,
    _In_ PPH_MINIMETER_DATA Meter
    )
{
    PH_GRAPH_DRAW_INFO drawInfo;
    FLOAT data[PH_MINIMETER_HISTORY];
    ULONG step = (ULONG)max(1, GwScale(2, Dpi));
    ULONG count = min((ULONG)Width / step + 1, PH_MINIMETER_HISTORY);
    FLOAT scale = Meter->Limit;
    COLORREF color = Meter->Color;
    ULONG i;

    if (scale <= 0)
    {
        for (i = 0; i < count; i++)
            scale = max(scale, Meter->History[i]);

        scale = max(scale, Meter->MinScale);
    }

    if (scale <= 0)
        scale = 1;

    for (i = 0; i < count; i++)
    {
        FLOAT fraction = Meter->History[i] / scale;

        data[i] = fraction > 1 ? 1 : fraction;
    }

    memset(&drawInfo, 0, sizeof(drawInfo));
    drawInfo.Width = Width;
    drawInfo.Height = Height;
    drawInfo.Step = step;
    drawInfo.LineDataCount = count;
    drawInfo.LineData1 = data;

    // Same palette rules as the mini meters and the Performance tab.
    switch (PhSipGetGraphColorMode())
    {
    case 2:
        drawInfo.BackColor = PhThemeWindowForegroundColor;
        drawInfo.LineColor1 = color;
        drawInfo.LineBackColor1 = PhHalveColorBrightness(color);
        break;
    case 1:
        drawInfo.BackColor = RGB(0x00, 0x00, 0x00);
        drawInfo.LineColor1 = color;
        drawInfo.LineBackColor1 = PhHalveColorBrightness(color);
        break;
    default:
        drawInfo.BackColor = RGB(0xef, 0xef, 0xef);
        drawInfo.LineColor1 = PhHalveColorBrightness(color);
        drawInfo.LineBackColor1 = PhMakeColorBrighter(color, 125);
        break;
    }

    PhDrawGraphDirect(Dc, Bits, &drawInfo);
}

static VOID GwPaint(
    _In_ HWND WindowHandle,
    _In_ ULONG Index,
    _In_ HDC WindowDc
    )
{
    PH_MINIMETER_DATA meter;
    RECT client;
    LONG dpi = PhGetWindowDpi(WindowHandle);
    LONG margin = GwScale(GW_MARGIN, dpi);
    LONG header = GwScale(GW_HEADER, dpi);
    HDC bufferDc;
    HBITMAP bufferBitmap;
    HGDIOBJ bufferOld;
    HFONT font;
    HGDIOBJ oldFont;
    RECT headerRect;
    RECT graphRect;
    LONG graphW;
    LONG graphH;

    if (!PhMiniMetersGetMeter(Index, &meter))
        return;

    GetClientRect(WindowHandle, &client);

    if (client.right <= 0 || client.bottom <= 0)
        return;

    graphRect.left = margin;
    graphRect.top = header;
    graphRect.right = client.right - margin;
    graphRect.bottom = client.bottom - margin;
    graphW = graphRect.right - graphRect.left;
    graphH = graphRect.bottom - graphRect.top;

    bufferDc = CreateCompatibleDC(WindowDc);
    bufferBitmap = CreateCompatibleBitmap(WindowDc, client.right, client.bottom);

    if (!bufferDc || !bufferBitmap)
    {
        if (bufferBitmap)
            DeleteObject(bufferBitmap);
        if (bufferDc)
            DeleteDC(bufferDc);
        return;
    }

    bufferOld = SelectObject(bufferDc, bufferBitmap);

    font = (HFONT)SendMessage(PhMwpProcessTreeNewHandle, WM_GETFONT, 0, 0);

    if (!font)
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    oldFont = SelectObject(bufferDc, font);
    SetBkMode(bufferDc, TRANSPARENT);

    SetDCBrushColor(bufferDc, GwPanelColor());
    FillRect(bufferDc, &client, (HBRUSH)GetStockObject(DC_BRUSH));

    headerRect.left = margin;
    headerRect.top = 0;
    headerRect.right = client.right - margin;
    headerRect.bottom = header;

    SetTextColor(bufferDc, meter.Available ? GwTextColor() : GwDisabledColor());
    DrawText(bufferDc, meter.Title, -1, &headerRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    DrawText(bufferDc, meter.Available ? meter.Text : L"n/a", -1, &headerRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (graphW > 0 && graphH > 0)
    {
        if (meter.Available)
        {
            BITMAPINFO info;
            PVOID bits = NULL;
            HDC graphDc = CreateCompatibleDC(WindowDc);
            HBITMAP graphBitmap;

            memset(&info, 0, sizeof(info));
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = graphW;
            info.bmiHeader.biHeight = -graphH; // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            graphBitmap = CreateDIBSection(WindowDc, &info, DIB_RGB_COLORS, &bits, NULL, 0);

            if (graphDc && graphBitmap && bits)
            {
                HGDIOBJ graphOld = SelectObject(graphDc, graphBitmap);

                GwDrawGraph(graphDc, bits, graphW, graphH, dpi, &meter);
                BitBlt(bufferDc, graphRect.left, graphRect.top, graphW, graphH, graphDc, 0, 0, SRCCOPY);
                SelectObject(graphDc, graphOld);
            }

            if (graphBitmap)
                DeleteObject(graphBitmap);
            if (graphDc)
                DeleteDC(graphDc);
        }
        else
        {
            SetDCBrushColor(bufferDc, PhEnableThemeSupport ? PhThemeWindowBackground2Color : GetSysColor(COLOR_3DFACE));
            FillRect(bufferDc, &graphRect, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    }

    SelectObject(bufferDc, oldFont);
    BitBlt(WindowDc, 0, 0, client.right, client.bottom, bufferDc, 0, 0, SRCCOPY);
    SelectObject(bufferDc, bufferOld);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
}

// Window

static VOID GwSetTopMost(
    _In_ ULONG Index,
    _In_ BOOLEAN TopMost
    )
{
    GwState[Index].TopMost = TopMost;

    if (GwState[Index].Window)
    {
        SetWindowPos(
            GwState[Index].Window,
            TopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
            );
    }
}

static VOID GwShowContextMenu(
    _In_ HWND WindowHandle,
    _In_ ULONG Index,
    _In_ LPARAM Position
    )
{
    PPH_EMENU menu;
    PPH_EMENU_ITEM topMostItem;
    PPH_EMENU_ITEM item;
    POINT point;

    point.x = GET_X_LPARAM(Position);
    point.y = GET_Y_LPARAM(Position);

    if (point.x == -1 && point.y == -1)
    {
        point.x = point.y = 0;
        ClientToScreen(WindowHandle, &point);
    }

    menu = PhCreateEMenu();
    PhInsertEMenuItem(menu, topMostItem = PhCreateEMenuItem(0, GW_COMMAND_TOPMOST, L"&Always on top", NULL, NULL), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuSeparator(), ULONG_MAX);
    PhInsertEMenuItem(menu, PhCreateEMenuItem(0, GW_COMMAND_CLOSE, L"&Close", NULL, NULL), ULONG_MAX);

    if (GwState[Index].TopMost)
        topMostItem->Flags |= PH_EMENU_CHECKED;

    item = PhShowEMenu(menu, WindowHandle, PH_EMENU_SHOW_LEFTRIGHT, PH_ALIGN_LEFT | PH_ALIGN_TOP, point.x, point.y);

    if (item)
    {
        switch (item->Id)
        {
        case GW_COMMAND_TOPMOST:
            GwSetTopMost(Index, !GwState[Index].TopMost);
            GwSave();
            break;
        case GW_COMMAND_CLOSE:
            PostMessage(WindowHandle, WM_CLOSE, 0, 0);
            break;
        }
    }

    PhDestroyEMenu(menu);
}

static LRESULT CALLBACK GwWindowProc(
    _In_ HWND WindowHandle,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    ULONG index = (ULONG)GetWindowLongPtr(WindowHandle, GWLP_USERDATA);

    switch (Message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(WindowHandle, &ps);

            if (hdc && index < PH_MINIMETER_COUNT)
                GwPaint(WindowHandle, index, hdc);

            EndPaint(WindowHandle, &ps);
        }
        return 0;
    case WM_GETMINMAXINFO:
        {
            PMINMAXINFO info = (PMINMAXINFO)lParam;
            LONG dpi = PhGetWindowDpi(WindowHandle);
            RECT frame = { 0, 0, GwScale(GW_MIN_W, dpi), GwScale(GW_MIN_H, dpi) };

            AdjustWindowRectEx(&frame, (ULONG)GetWindowLongPtr(WindowHandle, GWL_STYLE), FALSE, (ULONG)GetWindowLongPtr(WindowHandle, GWL_EXSTYLE));
            info->ptMinTrackSize.x = frame.right - frame.left;
            info->ptMinTrackSize.y = frame.bottom - frame.top;
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(WindowHandle, NULL, FALSE);
        break;
    case WM_EXITSIZEMOVE:
        if (index < PH_MINIMETER_COUNT)
            GwSave();
        break;
    case WM_CONTEXTMENU:
        if (index < PH_MINIMETER_COUNT)
            GwShowContextMenu(WindowHandle, index, lParam);
        return 0;
    case WM_LBUTTONDBLCLK:
        // Double-click toggles always-on-top, a quick alternative to the context menu.
        if (index < PH_MINIMETER_COUNT)
        {
            GwSetTopMost(index, !GwState[index].TopMost);
            GwSave();
        }
        return 0;
    case WM_THEMECHANGED:
    case WM_DPICHANGED:
        InvalidateRect(WindowHandle, NULL, FALSE);
        break;
    case WM_CLOSE:
        // A user close forgets the "open" flag; a process exit never gets here.
        if (index < PH_MINIMETER_COUNT)
        {
            GwCaptureRects();
            GwState[index].Open = FALSE;
            GwSave();
        }
        DestroyWindow(WindowHandle);
        return 0;
    case WM_DESTROY:
        if (index < PH_MINIMETER_COUNT)
            GwState[index].Window = NULL;
        break;
    }

    return DefWindowProc(WindowHandle, Message, wParam, lParam);
}

static BOOLEAN GwEnsureClass(VOID)
{
    WNDCLASSEX wc;

    if (GwClassRegistered)
        return TRUE;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = GwWindowProc;
    wc.hInstance = PhInstanceHandle;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = GW_CLASS_NAME;

    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;

    GwClassRegistered = TRUE;
    return TRUE;
}

static VOID GwOpen(
    _In_ ULONG Index
    )
{
    GW_STATE *state = &GwState[Index];
    PH_MINIMETER_DATA meter;
    RECT rect;
    LONG dpi = PhGetWindowDpi(PhMainWndHandle);
    HWND window;
    int x = CW_USEDEFAULT;
    int y = 0;
    int width;
    int height;

    if (state->Window || !GwEnsureClass() || !PhMiniMetersGetMeter(Index, &meter))
        return;

    // Window size is the outer size, so convert the default client size.
    rect.left = rect.top = 0;
    rect.right = GwScale(GW_DEFAULT_W, dpi);
    rect.bottom = GwScale(GW_DEFAULT_H, dpi);
    AdjustWindowRectEx(&rect, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, WS_EX_TOOLWINDOW);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    if (state->HasRect)
    {
        rect = state->Rect;
        GwClampToMonitor(&rect);
        x = rect.left;
        y = rect.top;
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
    }

    // No owner: owned windows would vanish with the main window when it hides to the tray.
    window = CreateWindowEx(
        WS_EX_TOOLWINDOW | (state->TopMost ? WS_EX_TOPMOST : 0),
        GW_CLASS_NAME,
        meter.Title,
        WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        x, y, width, height,
        NULL,
        NULL,
        PhInstanceHandle,
        NULL
        );

    if (!window)
        return;

    SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)Index);
    state->Window = window;
    state->Open = TRUE;

    if (PhEnableThemeSupport)
        PhInitializeThemeWindowFrame(window);

    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    GwSave();
}

// Public interface

VOID PhDetachedGraphsInitialization(
    VOID
    )
{
    GwLoad();

    for (ULONG i = 0; i < PH_MINIMETER_COUNT; i++)
    {
        if (GwState[i].Open)
            GwOpen(i);
    }
}

VOID PhDetachedGraphsUpdate(
    VOID
    )
{
    BOOLEAN themeChanged = GwLastBackground != GwPanelColor();

    GwLastBackground = GwPanelColor();

    for (ULONG i = 0; i < PH_MINIMETER_COUNT; i++)
    {
        if (!GwState[i].Window)
            continue;

        if (themeChanged && PhEnableThemeSupport)
            PhInitializeThemeWindowFrame(GwState[i].Window); // caption colour follows the palette

        InvalidateRect(GwState[i].Window, NULL, FALSE);
    }
}

VOID PhToggleDetachedGraph(
    _In_ ULONG Index
    )
{
    if (Index >= PH_MINIMETER_COUNT)
        return;

    if (GwState[Index].Window)
    {
        SendMessage(GwState[Index].Window, WM_CLOSE, 0, 0);
    }
    else
    {
        GwOpen(Index);
    }
}

BOOLEAN PhIsDetachedGraphOpen(
    _In_ ULONG Index
    )
{
    return Index < PH_MINIMETER_COUNT && GwState[Index].Window != NULL;
}
