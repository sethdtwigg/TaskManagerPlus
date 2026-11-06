/*
 * TaskManagerPlus: mini meters.
 *
 * A child window of the main window, raised above the process list and anchored to its
 * bottom-right corner: 2 columns x 3 rows of flat rounded tiles (VRAM, GPU / Network, Disk /
 * Memory, CPU). Each tile shows a title, the current value and a history graph drawn with the
 * normal graph drawing code (PhDrawGraphDirect) in the series colours of the Performance tab.
 *
 * Data: CPU and memory come from the process provider globals; GPU, VRAM, network and disk come
 * from the ExtendedTools plugin interface (GetSystemSummary, interface version 4). When the plugin
 * or a measurement is missing the tile is greyed and reads "n/a".
 *
 * Sizes are multiples of 8 px at 96 dpi and scale with the window DPI. The panel is repositioned
 * after each main window layout pass (see PhMiniMetersRequestLayout), is hidden when the Processes
 * tab is not shown or the list is too small (it must not cover the column headers, the totals
 * row or the count pill), and passes mouse input through to the list. It repaints only from the
 * existing process-refresh callback and paints through a back buffer, so there is no timer and no
 * flicker.
 */

#include <phapp.h>
#include <phplug.h>
#include <phsettings.h>
#include <settings.h>
#include <guisup.h>
#include <mainwnd.h>
#include <mainwndp.h>
#include <sysinfop.h>
#include <procprv.h>
#include <treenew.h>
#include <minimeters.h>

#define MM_CLASS_NAME L"PhMiniMeters"
#define WM_MM_LAYOUT (WM_APP + 0x3a1)

#define MM_COLUMNS 2
#define MM_ROWS 3
#define MM_COUNT (MM_COLUMNS * MM_ROWS)
#define MM_HISTORY 160

// Layout in pixels at 96 dpi.
#define MM_MARGIN 8
#define MM_TILE_W 136
#define MM_TILE_H 64
#define MM_GAP 8
#define MM_TITLE_H 16
#define MM_RADIUS 8
#define MM_LIST_RESERVE_W 240 // keep the left part of the list (count pill) free

// Matches EXTENDEDTOOLS_SYSTEM_SUMMARY / EXTENDEDTOOLS_INTERFACE in plugins\include\extendedtoolsintf.h
// (version 4). The plugin headers are not on the core include path, so the layout is repeated here.
#define MM_ET_PLUGIN_NAME L"ExtendedTools"
#define MM_ET_INTERFACE_VERSION 4

typedef struct _MM_ET_SUMMARY
{
    BOOLEAN HaveGpu;
    BOOLEAN HaveDisk;
    BOOLEAN HaveNetwork;
    FLOAT GpuUtilization;
    ULONG64 DedicatedBytes;
    ULONG64 DedicatedLimitBytes;
    ULONG64 DiskReadRate;
    ULONG64 DiskWriteRate;
    ULONG64 NetworkReceiveRate;
    ULONG64 NetworkSendRate;
} MM_ET_SUMMARY;

typedef struct _MM_ET_INTERFACE
{
    ULONG Version;
    PVOID Reserved[7]; // GPU adapter and per-process lookups
    VOID (NTAPI *GetSystemSummary)(_Out_ MM_ET_SUMMARY *Summary);
} MM_ET_INTERFACE;

typedef enum _MM_METER_ID
{
    MmVram,
    MmGpu,
    MmNetwork,
    MmDisk,
    MmMemory,
    MmCpu
} MM_METER_ID;

typedef struct _MM_METER
{
    PCWSTR Title;
    BOOLEAN Available;
    BOOLEAN Percent;   // value is a 0..1 fraction shown as a percentage
    BOOLEAN Rate;      // value is bytes per second
    FLOAT Limit;       // fixed graph maximum; 0 = scale to the history (never below MinScale)
    FLOAT MinScale;
    FLOAT Value;       // newest sample
    WCHAR Text[32];
    FLOAT History[MM_HISTORY]; // [0] = newest
} MM_METER;

BOOLEAN PhMiniMetersEnabled = TRUE;

static MM_METER MmMeters[MM_COUNT];
static HWND MmWindow = NULL;
static HWND MmMainWindow = NULL;
static BOOLEAN MmLayoutPending = FALSE;
static MM_ET_INTERFACE *MmEtInterface = NULL;

static HDC MmBufferDc = NULL;
static HBITMAP MmBufferBitmap = NULL;
static HGDIOBJ MmBufferOld = NULL;
static HDC MmGraphDc = NULL;
static HBITMAP MmGraphBitmap = NULL;
static HGDIOBJ MmGraphOld = NULL;
static PVOID MmGraphBits = NULL;
static LONG MmBufferW = 0;
static LONG MmBufferH = 0;
static LONG MmGraphW = 0;
static LONG MmGraphH = 0;

static LONG MmScale(
    _In_ LONG Value,
    _In_ LONG Dpi
    )
{
    return MulDiv(Value, Dpi, 96);
}

static COLORREF MmPanelColor(VOID)
{
    return PhEnableThemeSupport ? PhThemeWindowBackgroundColor : GetSysColor(COLOR_WINDOW);
}

static COLORREF MmTileColor(VOID)
{
    return PhEnableThemeSupport ? PhThemeWindowBackground2Color : GetSysColor(COLOR_3DFACE);
}

static COLORREF MmTextColor(VOID)
{
    return PhEnableThemeSupport ? PhThemeWindowTextColor : GetSysColor(COLOR_WINDOWTEXT);
}

static COLORREF MmBlend(
    _In_ COLORREF A,
    _In_ COLORREF B
    )
{
    return RGB(
        (GetRValue(A) + GetRValue(B)) / 2,
        (GetGValue(A) + GetGValue(B)) / 2,
        (GetBValue(A) + GetBValue(B)) / 2
        );
}

static COLORREF MmSeriesColor(
    _In_ ULONG Meter
    )
{
    switch (Meter)
    {
    case MmVram: return PhCsColorPrivate;
    case MmGpu: return PhCsColorCpuKernel;
    case MmNetwork: return PhCsColorIoWrite;
    case MmDisk: return PhCsColorIoReadOther;
    case MmMemory: return PhCsColorPhysical;
    default: return PhCsColorCpuUser;
    }
}

static VOID MmInitializeMeters(VOID)
{
    memset(MmMeters, 0, sizeof(MmMeters));

    MmMeters[MmVram].Title = L"VRAM";
    MmMeters[MmGpu].Title = L"GPU";
    MmMeters[MmNetwork].Title = L"Network";
    MmMeters[MmDisk].Title = L"Disk";
    MmMeters[MmMemory].Title = L"Memory";
    MmMeters[MmCpu].Title = L"CPU";

    MmMeters[MmGpu].Percent = TRUE;
    MmMeters[MmGpu].Limit = 1.0f;
    MmMeters[MmNetwork].Rate = TRUE;
    MmMeters[MmNetwork].MinScale = 1024.0f * 1024.0f;
    MmMeters[MmDisk].Rate = TRUE;
    MmMeters[MmDisk].MinScale = 1024.0f * 1024.0f;
    MmMeters[MmCpu].Percent = TRUE;
    MmMeters[MmCpu].Limit = 1.0f;
}

static VOID MmSetSample(
    _In_ ULONG Meter,
    _In_ BOOLEAN Available,
    _In_ FLOAT Value,
    _In_ FLOAT Limit
    )
{
    MM_METER *meter = &MmMeters[Meter];
    PH_FORMAT format[3];
    SIZE_T returnLength;

    meter->Available = Available;
    meter->Text[0] = UNICODE_NULL;

    if (!Available)
        return;

    if (Value < 0)
        Value = 0;

    if (meter->Percent)
    {
        if (Value > 1)
            Value = 1;

        PhInitFormatF(&format[0], Value * 100.0f, 1);
        PhInitFormatC(&format[1], L'%');
        PhFormatToBuffer(format, 2, meter->Text, sizeof(meter->Text), &returnLength);
    }
    else
    {
        PhInitFormatSize(&format[0], (ULONG64)Value);

        if (meter->Rate)
            PhInitFormatS(&format[1], L"/s");

        PhFormatToBuffer(format, meter->Rate ? 2 : 1, meter->Text, sizeof(meter->Text), &returnLength);

        if (Limit > 0)
            meter->Limit = Limit; // memory and VRAM have a known total
    }

    meter->Value = Value;
    memmove(&meter->History[1], &meter->History[0], (MM_HISTORY - 1) * sizeof(FLOAT));
    meter->History[0] = Value;
}

static VOID MmSample(VOID)
{
    MM_ET_SUMMARY summary;
    ULONG64 pageSize = PhSystemBasicInformation.PageSize;
    ULONG64 total = (ULONG64)PhSystemBasicInformation.NumberOfPhysicalPages * pageSize;
    ULONG64 available = (ULONG64)PhPerfInformation.AvailablePages * pageSize;

    if (!MmEtInterface)
    {
        PVOID iface = PhGetPluginInterfaceZ(MM_ET_PLUGIN_NAME, MM_ET_INTERFACE_VERSION);

        if (iface && ((MM_ET_INTERFACE *)iface)->GetSystemSummary)
            MmEtInterface = iface;
    }

    memset(&summary, 0, sizeof(summary));

    if (MmEtInterface)
        MmEtInterface->GetSystemSummary(&summary);

    MmSetSample(MmCpu, TRUE, PhCpuKernelUsage + PhCpuUserUsage, 0);
    MmSetSample(MmMemory, total != 0, (FLOAT)(total > available ? total - available : 0), (FLOAT)total);
    MmSetSample(MmGpu, summary.HaveGpu, summary.GpuUtilization, 0);
    MmSetSample(MmVram, summary.HaveGpu && summary.DedicatedLimitBytes != 0, (FLOAT)summary.DedicatedBytes, (FLOAT)summary.DedicatedLimitBytes);
    MmSetSample(MmNetwork, summary.HaveNetwork, (FLOAT)(summary.NetworkReceiveRate + summary.NetworkSendRate), 0);
    MmSetSample(MmDisk, summary.HaveDisk, (FLOAT)(summary.DiskReadRate + summary.DiskWriteRate), 0);
}

static VOID MmGetPanelSize(
    _In_ LONG Dpi,
    _Out_ PSIZE Size
    )
{
    Size->cx = MmScale(MM_MARGIN * 2 + MM_COLUMNS * MM_TILE_W + (MM_COLUMNS - 1) * MM_GAP, Dpi);
    Size->cy = MmScale(MM_MARGIN * 2 + MM_ROWS * MM_TILE_H + (MM_ROWS - 1) * MM_GAP, Dpi);
}

static VOID MmFreeBuffers(VOID)
{
    if (MmBufferDc)
    {
        SelectObject(MmBufferDc, MmBufferOld);
        DeleteDC(MmBufferDc);
        MmBufferDc = NULL;
    }
    if (MmBufferBitmap)
    {
        DeleteObject(MmBufferBitmap);
        MmBufferBitmap = NULL;
    }
    if (MmGraphDc)
    {
        SelectObject(MmGraphDc, MmGraphOld);
        DeleteDC(MmGraphDc);
        MmGraphDc = NULL;
    }
    if (MmGraphBitmap)
    {
        DeleteObject(MmGraphBitmap);
        MmGraphBitmap = NULL;
    }

    MmGraphBits = NULL;
    MmBufferW = MmBufferH = MmGraphW = MmGraphH = 0;
}

static BOOLEAN MmEnsureBuffers(
    _In_ HDC WindowDc,
    _In_ LONG Width,
    _In_ LONG Height,
    _In_ LONG GraphWidth,
    _In_ LONG GraphHeight
    )
{
    if (MmBufferDc && MmBufferW == Width && MmBufferH == Height && MmGraphW == GraphWidth && MmGraphH == GraphHeight)
        return TRUE;

    MmFreeBuffers();

    MmBufferDc = CreateCompatibleDC(WindowDc);
    MmBufferBitmap = CreateCompatibleBitmap(WindowDc, Width, Height);

    if (MmBufferDc && MmBufferBitmap)
    {
        BITMAPINFO info;

        MmBufferOld = SelectObject(MmBufferDc, MmBufferBitmap);
        MmBufferW = Width;
        MmBufferH = Height;

        memset(&info, 0, sizeof(info));
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = GraphWidth;
        info.bmiHeader.biHeight = -GraphHeight; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        MmGraphDc = CreateCompatibleDC(WindowDc);
        MmGraphBitmap = CreateDIBSection(WindowDc, &info, DIB_RGB_COLORS, &MmGraphBits, NULL, 0);

        if (MmGraphDc && MmGraphBitmap && MmGraphBits)
        {
            MmGraphOld = SelectObject(MmGraphDc, MmGraphBitmap);
            MmGraphW = GraphWidth;
            MmGraphH = GraphHeight;
            return TRUE;
        }
    }

    MmFreeBuffers();
    return FALSE;
}

static VOID MmDrawGraph(
    _In_ ULONG Index,
    _In_ LONG Dpi
    )
{
    MM_METER *meter = &MmMeters[Index];
    PH_GRAPH_DRAW_INFO drawInfo;
    FLOAT data[MM_HISTORY];
    COLORREF color = MmSeriesColor(Index);
    ULONG step = (ULONG)max(1, MmScale(2, Dpi));
    ULONG count = min((ULONG)MmGraphW / step + 1, MM_HISTORY);
    FLOAT scale = meter->Limit;
    ULONG i;

    if (scale <= 0)
    {
        for (i = 0; i < count; i++)
            scale = max(scale, meter->History[i]);

        scale = max(scale, meter->MinScale);
    }

    if (scale <= 0)
        scale = 1;

    for (i = 0; i < count; i++)
    {
        FLOAT fraction = meter->History[i] / scale;

        data[i] = fraction > 1 ? 1 : fraction;
    }

    memset(&drawInfo, 0, sizeof(drawInfo));
    drawInfo.Width = MmGraphW;
    drawInfo.Height = MmGraphH;
    drawInfo.Step = step;
    drawInfo.LineDataCount = count;
    drawInfo.LineData1 = data;

    // Same palette rules as the Performance tab (sysinfo.c PhSiSetColorsGraphDrawInfo).
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

    PhDrawGraphDirect(MmGraphDc, MmGraphBits, &drawInfo);
}

static VOID MmPaint(
    _In_ HWND WindowHandle,
    _In_ HDC WindowDc
    )
{
    RECT client;
    LONG dpi = PhGetWindowDpi(WindowHandle);
    LONG margin = MmScale(MM_MARGIN, dpi);
    LONG tileW = MmScale(MM_TILE_W, dpi);
    LONG tileH = MmScale(MM_TILE_H, dpi);
    LONG gap = MmScale(MM_GAP, dpi);
    LONG titleH = MmScale(MM_TITLE_H, dpi);
    LONG radius = MmScale(MM_RADIUS, dpi);
    LONG graphW = tileW - margin * 2;
    LONG graphH = tileH - titleH - margin - MmScale(4, dpi);
    COLORREF tileColor = MmTileColor();
    COLORREF textColor = MmTextColor();
    COLORREF greyColor = MmBlend(textColor, tileColor);
    HFONT font;
    HGDIOBJ oldFont;
    ULONG i;

    GetClientRect(WindowHandle, &client);

    if (client.right <= 0 || client.bottom <= 0 || graphW <= 0 || graphH <= 0)
        return;

    if (!MmEnsureBuffers(WindowDc, client.right, client.bottom, graphW, graphH))
        return;

    font = (HFONT)SendMessage(PhMwpProcessTreeNewHandle, WM_GETFONT, 0, 0);

    if (!font)
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    oldFont = SelectObject(MmBufferDc, font);
    SetBkMode(MmBufferDc, TRANSPARENT);

    SetDCBrushColor(MmBufferDc, MmPanelColor());
    FillRect(MmBufferDc, &client, (HBRUSH)GetStockObject(DC_BRUSH));

    for (i = 0; i < MM_COUNT; i++)
    {
        MM_METER *meter = &MmMeters[i];
        RECT tile;
        RECT titleRect;
        RECT valueRect;
        RECT graphRect;
        HGDIOBJ oldBrush;
        HGDIOBJ oldPen;

        tile.left = margin + (LONG)(i % MM_COLUMNS) * (tileW + gap);
        tile.top = margin + (LONG)(i / MM_COLUMNS) * (tileH + gap);
        tile.right = tile.left + tileW;
        tile.bottom = tile.top + tileH;

        SetDCBrushColor(MmBufferDc, tileColor);
        SetDCPenColor(MmBufferDc, tileColor);
        oldBrush = SelectObject(MmBufferDc, GetStockObject(DC_BRUSH));
        oldPen = SelectObject(MmBufferDc, GetStockObject(DC_PEN));
        RoundRect(MmBufferDc, tile.left, tile.top, tile.right, tile.bottom, radius, radius);
        SelectObject(MmBufferDc, oldPen);
        SelectObject(MmBufferDc, oldBrush);

        titleRect.left = tile.left + margin;
        titleRect.right = tile.right - margin;
        titleRect.top = tile.top + MmScale(4, dpi);
        titleRect.bottom = titleRect.top + titleH;
        valueRect = titleRect;

        SetTextColor(MmBufferDc, meter->Available ? textColor : greyColor);
        DrawText(MmBufferDc, meter->Title, -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        DrawText(MmBufferDc, meter->Available ? meter->Text : L"n/a", -1, &valueRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        graphRect.left = tile.left + margin;
        graphRect.top = titleRect.bottom + MmScale(2, dpi);
        graphRect.right = graphRect.left + graphW;
        graphRect.bottom = graphRect.top + graphH;

        if (meter->Available)
        {
            MmDrawGraph(i, dpi);
            BitBlt(MmBufferDc, graphRect.left, graphRect.top, graphW, graphH, MmGraphDc, 0, 0, SRCCOPY);
        }
        else
        {
            SetDCBrushColor(MmBufferDc, MmBlend(tileColor, MmPanelColor()));
            FillRect(MmBufferDc, &graphRect, (HBRUSH)GetStockObject(DC_BRUSH));
        }
    }

    SelectObject(MmBufferDc, oldFont);
    BitBlt(WindowDc, 0, 0, client.right, client.bottom, MmBufferDc, 0, 0, SRCCOPY);
}

static VOID MmApplyLayout(VOID)
{
    RECT tree;
    RECT header;
    SIZE size;
    LONG dpi;
    LONG margin;
    LONG headerHeight;
    BOOLEAN show = FALSE;

    MmLayoutPending = FALSE;

    if (!MmWindow || !MmMainWindow)
        return;

    if (PhMiniMetersEnabled &&
        PhMwpProcessTreeNewHandle &&
        FlagOn((ULONG)GetWindowLongPtr(PhMwpProcessTreeNewHandle, GWL_STYLE), WS_VISIBLE) &&
        GetClientRect(PhMwpProcessTreeNewHandle, &tree))
    {
        dpi = PhGetWindowDpi(MmMainWindow);
        margin = MmScale(MM_MARGIN, dpi);
        MmGetPanelSize(dpi, &size);

        headerHeight = 0;

        if (GetWindowRect(TreeNew_GetFixedHeader(PhMwpProcessTreeNewHandle), &header))
            headerHeight = header.bottom - header.top;

        if (headerHeight <= 0)
            headerHeight = MmScale(24, dpi);

        // Headers + totals row + one row of air stay uncovered; so does the left part of the list.
        if (tree.bottom - tree.top >= headerHeight * 3 + margin + size.cy + margin &&
            tree.right - tree.left >= size.cx + margin + MmScale(MM_LIST_RESERVE_W, dpi))
        {
            HRGN region;

            MapWindowPoints(PhMwpProcessTreeNewHandle, MmMainWindow, (PPOINT)&tree, 2);
            show = TRUE;

            SetWindowPos(
                MmWindow,
                HWND_TOP,
                tree.right - margin - size.cx,
                tree.bottom - margin - size.cy,
                size.cx,
                size.cy,
                SWP_NOACTIVATE | SWP_SHOWWINDOW
                );

            region = CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, MmScale(MM_RADIUS * 2, dpi), MmScale(MM_RADIUS * 2, dpi));

            if (region && !SetWindowRgn(MmWindow, region, FALSE))
                DeleteObject(region);

            InvalidateRect(MmWindow, NULL, FALSE);
        }
    }

    if (!show)
        ShowWindow(MmWindow, SW_HIDE);
}

static LRESULT CALLBACK MmWindowProc(
    _In_ HWND WindowHandle,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    switch (Message)
    {
    case WM_MM_LAYOUT:
        MmApplyLayout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT; // the list underneath keeps its mouse handling
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(WindowHandle, &ps);

            if (hdc)
                MmPaint(WindowHandle, hdc);

            EndPaint(WindowHandle, &ps);
        }
        return 0;
    case WM_DPICHANGED:
    case WM_THEMECHANGED:
        MmFreeBuffers();
        break;
    case WM_DESTROY:
        MmFreeBuffers();
        MmWindow = NULL;
        break;
    }

    return DefWindowProc(WindowHandle, Message, wParam, lParam);
}

VOID PhMiniMetersInitialization(
    _In_ HWND MainWindowHandle
    )
{
    WNDCLASSEX wc;

    MmInitializeMeters();
    PhMiniMetersEnabled = !!PhGetIntegerSetting(SETTING_MINI_METERS);
    MmMainWindow = MainWindowHandle;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MmWindowProc;
    wc.hInstance = PhInstanceHandle;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = MM_CLASS_NAME;

    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    MmWindow = CreateWindowEx(
        0,
        MM_CLASS_NAME,
        NULL,
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        MainWindowHandle,
        NULL,
        PhInstanceHandle,
        NULL
        );
}

VOID PhMiniMetersRequestLayout(
    VOID
    )
{
    if (MmWindow && !MmLayoutPending)
    {
        MmLayoutPending = TRUE;
        PostMessage(MmWindow, WM_MM_LAYOUT, 0, 0);
    }
}

VOID PhMiniMetersUpdate(
    VOID
    )
{
    if (!MmWindow)
        return;

    MmSample();

    if (IsWindowVisible(MmWindow))
        InvalidateRect(MmWindow, NULL, FALSE);
}

VOID PhToggleMiniMeters(
    VOID
    )
{
    PhMiniMetersEnabled = !PhMiniMetersEnabled;
    PhSetIntegerSetting(SETTING_MINI_METERS, PhMiniMetersEnabled);
    MmApplyLayout();
}
