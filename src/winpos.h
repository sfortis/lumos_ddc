#ifndef WINPOS_H
#define WINPOS_H

#include <windows.h>
#include "layout.h"

/* Reads and moves the windows of other applications for the window restore.
 * The decisions are in layout.c; this module only talks to Windows.
 *
 * Every call runs with a per-monitor DPI context on the calling thread.
 * Lumos itself is not DPI aware, so without it Windows would hand out scaled
 * coordinates for other applications' windows. */

typedef struct { int restored, gone, touched, unchanged, failed, hung, hidden; } WinposReport;

/* The connected monitors with their device interface paths and rects. */
void Winpos_ReadDisplays(LayoutDisplays *out);

/* The displays and every window that qualifies (visible, unowned, not ours,
   not the shell). */
void Winpos_Capture(LayoutSnapshot *snap);

/* Post msg to notify each time the user finishes moving, sizing, minimizing
   or restoring a window, so that the rolling snapshot can be refreshed. */
void Winpos_Watch(HWND notify, UINT msg);
void Winpos_Unwatch(void);

/* Record windows the user moves, sizes, minimizes or restores from now on.
   Starting again clears the record. Runs on the calling thread's message loop. */
void Winpos_StartTracking(void);
void Winpos_StopTracking(void);

/* Put every saved window back unless layout.c says to skip it. Neither the
   focus nor the z-order changes. */
void Winpos_Restore(const LayoutSnapshot *snap, WinposReport *rep);

/* Diagnostics for the debug build. With a log function set, the module writes
   every touched window, every restore decision, and on request the windows
   of a snapshot and the ones that moved again after a restore. */
typedef void (*WinposLogFn)(const char *fmt, ...);
void Winpos_SetLog(WinposLogFn fn);
void Winpos_LogSnapshot(const LayoutSnapshot *snap, const char *what);
void Winpos_Verify(const LayoutSnapshot *snap);

#endif /* WINPOS_H */
