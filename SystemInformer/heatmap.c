/*
 * TaskManagerPlus: process list heat map, power usage rating and count pill.
 *
 * Heat map: a cell is tinted in proportion to its value. The value is mapped to a 0..1 intensity
 * with a ramp (see PhpHeatRamp), then the cell colour is the row background blended towards the
 * palette accent (PhThemeWindowFocusBorderColor); the hottest values shift towards a warm
 * orange. The blend strength is 10%..60%. Text keeps the normal colour unless that gives less than
 * 4.5:1 contrast (WCAG relative luminance), then black or white is used, whichever is higher.
 * Because the row background and the accent both come from the active palette, the same code
 * serves the light and the dark palette.
 *
 * Intensity thresholds (below "low" there is no tint at all):
 *   CPU      0.5% .. 50%            (square-root ramp)
 *   Memory   0.5% .. 20% of RAM     (square-root ramp)
 *   Disk     64 KB/s .. 64 MB/s     (logarithmic ramp)
 *   Network  8 KB/s .. 32 MB/s      (logarithmic ramp)
 *   GPU      1% .. 60%              (square-root ramp)
 *
 * Power usage: score = CPU% + 0.8 * GPU% + min(20, 2 * disk MB/s).
 *   score < 2   Very low
 *   score < 10  Low
 *   score < 30  Moderate
 *   otherwise   High
 * These are heuristics for ranking processes against each other, not a measurement of watts.
 */

#include <phapp.h>
#include <treenew.h>
#include <heatmap.h>

#include <math.h>

#define PH_HEAT_MIN_FRACTION 0.02
#define PH_HEAT_MIN_ALPHA 0.06 // TaskManagerPlus: subtle default (was 0.10 / 0.60)
#define PH_HEAT_MAX_ALPHA 0.35
#define PH_HEAT_WARM_START 0.70
#define PH_HEAT_WARM_COLOR RGB(0xe8, 0x91, 0x3a)

BOOLEAN PhHeatMapEnabled = TRUE;

static FLOAT PhpLinearTable[256];
static BOOLEAN PhpLinearTableReady = FALSE;

typedef enum _PH_HEAT_CURVE
{
    HeatCurveSqrt,
    HeatCurveLog
} PH_HEAT_CURVE;

static DOUBLE PhpHeatRamp(
    _In_ DOUBLE Value,
    _In_ DOUBLE Low,
    _In_ DOUBLE High,
    _In_ PH_HEAT_CURVE Curve
    )
{
    DOUBLE s;

    if (!(Value > Low))
        return 0;
    if (Value >= High)
        return 1;

    if (Curve == HeatCurveLog)
        s = log(Value / Low) / log(High / Low);
    else
        s = sqrt((Value - Low) / (High - Low));

    return s * s * (3 - 2 * s); // smoothstep
}

static DOUBLE PhpHeatGetFraction(
    _In_ ULONG Kind,
    _In_ DOUBLE Value
    )
{
    switch (Kind)
    {
    case PH_HEAT_KIND_CPU:
        return PhpHeatRamp(Value, 0.5, 50, HeatCurveSqrt);
    case PH_HEAT_KIND_MEMORY:
        {
            DOUBLE total = (DOUBLE)PhSystemBasicInformation.NumberOfPhysicalPages * PhSystemBasicInformation.PageSize;

            if (total <= 0)
                return 0;

            return PhpHeatRamp(Value / total * 100, 0.5, 20, HeatCurveSqrt);
        }
    case PH_HEAT_KIND_DISK:
        return PhpHeatRamp(Value, 64.0 * 1024, 64.0 * 1024 * 1024, HeatCurveLog);
    case PH_HEAT_KIND_NETWORK:
        return PhpHeatRamp(Value, 8.0 * 1024, 32.0 * 1024 * 1024, HeatCurveLog);
    case PH_HEAT_KIND_GPU:
        return PhpHeatRamp(Value, 1, 60, HeatCurveSqrt);
    }

    return 0;
}

static FLOAT PhpLuminance(
    _In_ COLORREF Color
    )
{
    if (!PhpLinearTableReady)
    {
        for (ULONG i = 0; i < 256; i++)
        {
            DOUBLE c = i / 255.0;

            PhpLinearTable[i] = (FLOAT)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
        }

        PhpLinearTableReady = TRUE;
    }

    return 0.2126f * PhpLinearTable[GetRValue(Color)] +
        0.7152f * PhpLinearTable[GetGValue(Color)] +
        0.0722f * PhpLinearTable[GetBValue(Color)];
}

static FLOAT PhpContrast(
    _In_ FLOAT LuminanceA,
    _In_ FLOAT LuminanceB
    )
{
    FLOAT hi = max(LuminanceA, LuminanceB);
    FLOAT lo = min(LuminanceA, LuminanceB);

    return (hi + 0.05f) / (lo + 0.05f);
}

static COLORREF PhpMix(
    _In_ COLORREF A,
    _In_ COLORREF B,
    _In_ DOUBLE T // 0 = A, 1 = B
    )
{
    return RGB(
        (BYTE)(GetRValue(A) + ((INT)GetRValue(B) - (INT)GetRValue(A)) * T + 0.5),
        (BYTE)(GetGValue(A) + ((INT)GetGValue(B) - (INT)GetGValue(A)) * T + 0.5),
        (BYTE)(GetBValue(A) + ((INT)GetBValue(B) - (INT)GetBValue(A)) * T + 0.5)
        );
}

BOOLEAN PhHeatGetColors(
    _In_ ULONG Kind,
    _In_ DOUBLE Value,
    _In_ COLORREF RowBackColor,
    _In_ COLORREF RowTextColor,
    _Out_ COLORREF *BackColor,
    _Out_ COLORREF *TextColor
    )
{
    DOUBLE fraction;
    COLORREF tint;
    COLORREF back;
    FLOAT backLuminance;

    fraction = PhpHeatGetFraction(Kind, Value);

    if (fraction < PH_HEAT_MIN_FRACTION)
        return FALSE;

    tint = PhThemeWindowFocusBorderColor; // the palette accent

    if (fraction > PH_HEAT_WARM_START)
        tint = PhpMix(tint, PH_HEAT_WARM_COLOR, (fraction - PH_HEAT_WARM_START) / (1 - PH_HEAT_WARM_START));

    back = PhpMix(RowBackColor, tint, PH_HEAT_MIN_ALPHA + (PH_HEAT_MAX_ALPHA - PH_HEAT_MIN_ALPHA) * fraction);
    backLuminance = PhpLuminance(back);

    *BackColor = back;
    *TextColor = RowTextColor;

    if (PhpContrast(PhpLuminance(RowTextColor), backLuminance) < 4.5f)
    {
        // Black or white, whichever reads better; one of them always reaches 4.5:1 or more.
        if (PhpContrast(0, backLuminance) >= PhpContrast(1, backLuminance))
            *TextColor = RGB(0, 0, 0);
        else
            *TextColor = RGB(0xff, 0xff, 0xff);
    }

    return TRUE;
}

ULONG PhPowerGetLevel(
    _In_ DOUBLE CpuPercent,
    _In_ DOUBLE GpuPercent,
    _In_ DOUBLE DiskBytesPerSecond
    )
{
    DOUBLE disk = DiskBytesPerSecond / (1024.0 * 1024.0) * 2;
    DOUBLE score;

    if (disk > 20)
        disk = 20;

    score = CpuPercent + 0.8 * GpuPercent + disk;

    if (score < 2)
        return PH_POWER_VERY_LOW;
    if (score < 10)
        return PH_POWER_LOW;
    if (score < 30)
        return PH_POWER_MODERATE;

    return PH_POWER_HIGH;
}

PCWSTR PhPowerGetLevelText(
    _In_ ULONG Level
    )
{
    switch (Level)
    {
    case PH_POWER_VERY_LOW:
        return L"Very low";
    case PH_POWER_LOW:
        return L"Low";
    case PH_POWER_MODERATE:
        return L"Moderate";
    }

    return L"High";
}

// The pill is laid out on the 8 pixel grid (scaled with the display DPI).
#define PH_PILL_MARGIN 8
#define PH_PILL_PADDING_X 12
#define PH_PILL_MIN_HEIGHT 24
#define PH_PILL_BAND_WIDTH 280

VOID PhHeatGetCountPillBand(
    _In_ PRECT ClientRect,
    _Out_ PRECT Band
    )
{
    Band->left = ClientRect->left;
    Band->right = min(ClientRect->right, ClientRect->left + PH_PILL_BAND_WIDTH);
    Band->bottom = ClientRect->bottom;
    Band->top = max(ClientRect->top, ClientRect->bottom - 64);
}

VOID PhHeatDrawCountPill(
    _In_ HDC Dc,
    _In_ PRECT ClientRect,
    _In_ ULONG Count
    )
{
    WCHAR buffer[48];
    PH_FORMAT format[2];
    SIZE_T returnLength;
    ULONG length;
    SIZE textSize;
    RECT pill;
    LONG dpi;
    LONG margin;
    LONG height;
    INT savedDc;

    PhInitFormatU(&format[0], Count);
    PhInitFormatS(&format[1], Count == 1 ? L" Process" : L" Processes");

    if (!PhFormatToBuffer(format, 2, buffer, sizeof(buffer), &returnLength))
        return;

    length = (ULONG)(returnLength / sizeof(WCHAR)) - 1; // minus the null terminator
    savedDc = SaveDC(Dc);

    if (!savedDc)
        return;

    dpi = GetDeviceCaps(Dc, LOGPIXELSY);
    margin = MulDiv(PH_PILL_MARGIN, dpi, 96);

    GetTextExtentPoint32(Dc, buffer, length, &textSize);

    height = max(MulDiv(PH_PILL_MIN_HEIGHT, dpi, 96), textSize.cy + margin);
    pill.left = ClientRect->left + margin;
    pill.bottom = ClientRect->bottom - margin;
    pill.top = pill.bottom - height;
    pill.right = pill.left + textSize.cx + 2 * MulDiv(PH_PILL_PADDING_X, dpi, 96);

    if (pill.top >= ClientRect->top && pill.right <= ClientRect->right)
    {
        SetDCBrushColor(Dc, PhThemeWindowBackground2Color);
        SetDCPenColor(Dc, PhThemeWindowBorderColor);
        SelectObject(Dc, PhGetStockBrush(DC_BRUSH));
        SelectObject(Dc, PhGetStockPen(DC_PEN));
        RoundRect(Dc, pill.left, pill.top, pill.right, pill.bottom, height, height);

        SetBkMode(Dc, TRANSPARENT);
        SetTextColor(Dc, PhThemeWindowTextColor);
        DrawText(Dc, buffer, length, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    RestoreDC(Dc, savedDc);
}
