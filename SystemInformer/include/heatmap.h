/*
 * TaskManagerPlus: process list heat map, power usage rating and count pill.
 *
 * Pure helpers: they turn a measured value into a tint (heat map), a rating (power usage) or a
 * drawn pill. Reading the values from the process nodes is done by proctree.c.
 */

#ifndef PH_HEATMAP_H
#define PH_HEATMAP_H

#define PH_POWER_VERY_LOW 0
#define PH_POWER_LOW 1
#define PH_POWER_MODERATE 2
#define PH_POWER_HIGH 3

extern BOOLEAN PhHeatMapEnabled; // View > Heat map (setting HeatMap)
extern ULONG PhHeatStyle; // Options > Highlighting (setting HeatStyle): 0 off, 1 subtle, 2 band

/**
 * Computes the tinted cell colours for a measured value.
 *
 * \param Kind One of PH_HEAT_KIND_*.
 * \param Value The measurement, in the unit given with the kind.
 * \param RowBackColor The colour the row is filled with.
 * \param RowTextColor The colour the text would normally be drawn with.
 * \param BackColor Receives the cell fill.
 * \param TextColor Receives the text colour (>= 4.5:1 against BackColor).
 *
 * \return FALSE when the value is too small for a visible tint.
 */
BOOLEAN PhHeatGetColors(
    _In_ ULONG Kind,
    _In_ DOUBLE Value,
    _In_ COLORREF RowBackColor,
    _In_ COLORREF RowTextColor,
    _Out_ COLORREF *BackColor,
    _Out_ COLORREF *TextColor
    );

// Rates a process as PH_POWER_*, from CPU %, GPU % and disk bytes per second.
ULONG PhPowerGetLevel(
    _In_ DOUBLE CpuPercent,
    _In_ DOUBLE GpuPercent,
    _In_ DOUBLE DiskBytesPerSecond
    );

PCWSTR PhPowerGetLevelText(
    _In_ ULONG Level
    );

// Draws the "N Processes" pill in the bottom-left corner of ClientRect.
VOID PhHeatDrawCountPill(
    _In_ HDC Dc,
    _In_ PRECT ClientRect,
    _In_ ULONG Count
    );

// The part of the client area the pill can occupy; invalidate it when the count changes.
VOID PhHeatGetCountPillBand(
    _In_ PRECT ClientRect,
    _Out_ PRECT Band
    );

#endif
