/*
 * TaskManagerPlus: mini meters, a small panel of live graphs over the bottom-right of the
 * Processes tab (VRAM, GPU, Network, Disk, Memory, CPU). See minimeters.c.
 */

#ifndef PH_MINIMETERS_H
#define PH_MINIMETERS_H

extern BOOLEAN PhMiniMetersEnabled; // View > Mini meters (setting MiniMeters)

// Creates the (initially hidden) panel as a child of the main window.
VOID PhMiniMetersInitialization(
    _In_ HWND MainWindowHandle
    );

// Takes one sample of every meter and repaints the panel. Call from the process-refresh callback.
VOID PhMiniMetersUpdate(
    VOID
    );

// Asks the panel to reposition itself once the pending window layout has been applied.
VOID PhMiniMetersRequestLayout(
    VOID
    );

VOID PhToggleMiniMeters(
    VOID
    );

// Read-only access to one meter (copy) for the detached graph windows (graphwnd.c).
#define PH_MINIMETER_COUNT 6
#define PH_MINIMETER_HISTORY 160

typedef struct _PH_MINIMETER_DATA
{
    PCWSTR Title;
    BOOLEAN Available;
    FLOAT Limit; // fixed graph maximum; 0 = scale to the history
    FLOAT MinScale;
    COLORREF Color;
    WCHAR Text[32];
    FLOAT History[PH_MINIMETER_HISTORY]; // [0] = newest
} PH_MINIMETER_DATA, *PPH_MINIMETER_DATA;

BOOLEAN PhMiniMetersGetMeter(
    _In_ ULONG Index,
    _Out_ PPH_MINIMETER_DATA Data
    );

#endif
