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

#endif
