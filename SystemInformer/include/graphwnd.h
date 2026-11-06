/*
 * TaskManagerPlus: detached graph windows. Any mini meter (CPU, Memory, Disk, Network, GPU,
 * VRAM) can be popped out into its own small always-on-top window (View > Detached graphs).
 * Position, size, the always-on-top flag and the open state are saved in the DetachedGraphs
 * setting. See graphwnd.c.
 */

#ifndef PH_GRAPHWND_H
#define PH_GRAPHWND_H

// Registers the window class and reopens the windows that were open at the last exit.
VOID PhDetachedGraphsInitialization(
    VOID
    );

// Repaints the open windows. Called by PhMiniMetersUpdate after each sample.
VOID PhDetachedGraphsUpdate(
    VOID
    );

// Opens the window for a mini meter index, or closes it when it is already open.
VOID PhToggleDetachedGraph(
    _In_ ULONG Index
    );

BOOLEAN PhIsDetachedGraphOpen(
    _In_ ULONG Index
    );

#endif
