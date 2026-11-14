#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdint.h>
#include <wchar.h>

/* Window arrangement snapshot and the rules for restoring it.
 *
 * When a display drops (a DisplayPort monitor asleep loses its link), Windows
 * moves every window onto the monitors that remain, or onto a small placeholder
 * when none remains, and leaves them there when the display returns. Lumos
 * takes a snapshot at lock or display-off and puts the windows back once the
 * same displays are present again.
 *
 * This module decides what to do; winpos.c reads and moves the windows. It is
 * Win32-free and has no clock (the caller owns the timers), so the whole state
 * machine is unit-tested in test_layout.c. */

#define LAYOUT_MAX_MONITORS 8
#define LAYOUT_MAX_WINDOWS  256
#define LAYOUT_ID_LEN       200
#define LAYOUT_CLASS_LEN    64

typedef struct { int left, top, right, bottom; } LayoutRect;

/* id is the monitor's device interface path. The GDI name (\\.\DISPLAY2) can
   change when a monitor drops and returns, the interface path does not. */
typedef struct { wchar_t id[LAYOUT_ID_LEN]; LayoutRect rect; } LayoutMonitor;
typedef struct { int count; LayoutMonitor mon[LAYOUT_MAX_MONITORS]; } LayoutDisplays;

enum { LAYOUT_SHOW_NORMAL = 0, LAYOUT_SHOW_MAXIMIZED, LAYOUT_SHOW_MINIMIZED };

typedef struct {
    uintptr_t     hwnd;
    unsigned long pid;
    wchar_t       cls[LAYOUT_CLASS_LEN];
    int           show;          /* LAYOUT_SHOW_* */
    int           restoreMax;    /* minimized window that restores to maximized */
    int           visible;       /* IsWindowVisible; a minimized window counts as visible */
    LayoutRect    normal;        /* rcNormalPosition, workspace coordinates */
    LayoutRect    actual;        /* GetWindowRect, screen coordinates */
} LayoutWindow;

typedef struct {
    LayoutDisplays displays;
    int            count;
    LayoutWindow   win[LAYOUT_MAX_WINDOWS];
} LayoutSnapshot;

/* TRUE when both lists hold the same monitors with the same rects, in any order. */
int Layout_SameDisplays(const LayoutDisplays *a, const LayoutDisplays *b);

enum { LAYOUT_RESTORE = 0, LAYOUT_SKIP_GONE, LAYOUT_SKIP_TOUCHED, LAYOUT_SKIP_UNCHANGED,
       LAYOUT_SKIP_HIDDEN };

/* What to do with one saved window. now is its current state, or NULL when the
   handle no longer names a window. touched is non-zero when the user moved,
   sized, minimized, maximized or restored it after the session became usable. */
int Layout_Decide(const LayoutWindow *saved, const LayoutWindow *now, int touched);

/* Whether a window event comes from the user. Windows raises the same events
   for its own moves: on 2026-11-06 it raised MINIMIZEEND for a minimized
   Firefox 170 ms after a monitor dropped, and the restore then skipped it as
   touched. An event counts only within LAYOUT_TOUCH_INPUT_MS of keyboard or
   mouse input, which a monitor switched off at its button never gives. A
   location change also needs the foreground window, because Windows moves
   every window at once and the user moves only the one in front. */
#define LAYOUT_TOUCH_INPUT_MS 1000
enum { LAYOUT_TOUCH_MOVESIZE = 0, LAYOUT_TOUCH_MINIMIZE, LAYOUT_TOUCH_LOCATION };
int Layout_IsUserTouch(int kind, int isForeground, unsigned long msSinceInput);

/* Actions returned by the event functions, as a bit mask, in the order the
   caller carries them out. */
#define LAYOUT_DO_SNAPSHOT  0x01   /* capture now and keep it */
#define LAYOUT_DO_TRACK     0x02   /* start recording windows the user touches */
#define LAYOUT_DO_QUIET     0x04   /* (re)start the quiet timer */
#define LAYOUT_DO_EXPIRY    0x08   /* (re)start the expiry timer */
#define LAYOUT_DO_RESTORE   0x10   /* apply the snapshot */
#define LAYOUT_DO_DISCARD   0x20   /* drop the snapshot, stop timers and tracking */
#define LAYOUT_DO_ADOPT     0x40   /* use the rolling snapshot as the snapshot; carried out first */

/* armed: a snapshot is held. disturbed: a display changed since it was taken.
   usable: the user can see and use the desktop (unlocked, display on), so the
   quiet period, the touched set and the expiry run. displayOff: Windows
   reported the display as off and has not reported it on again. */
typedef struct { int armed, disturbed, locked, usable, displayOff, rollReset; } LayoutState;

void Layout_Init(LayoutState *st);
int  Layout_OnLock(LayoutState *st);
int  Layout_OnUnlock(LayoutState *st);
int  Layout_OnDisplayOff(LayoutState *st);
int  Layout_OnDisplayOn(LayoutState *st);
/* WM_DISPLAYCHANGE or a monitor device arriving or leaving. haveRolling is
   non-zero when a rolling snapshot exists and was taken under other displays
   than the current ones: a monitor switched off at its button raises no lock
   and no display-off, so that snapshot is the only one taken before the drop.
   A change that leaves the displays as they were (an HDR toggle, a refresh
   rate) moves no window and must not restore an older arrangement. */
int  Layout_OnDisplayChange(LayoutState *st, int haveRolling);
int  Layout_OnQuiet(LayoutState *st, int sameDisplays);
int  Layout_OnExpiry(LayoutState *st);
int  Layout_OnDisable(LayoutState *st);

/* TRUE when the rolling snapshot may be refreshed: no snapshot is pending,
   the session is unlocked and the display is on. A refresh at any other time
   could hold the placeholder arrangement. */
int  Layout_CanRefresh(const LayoutState *st);

/* TRUE when a rolling capture taken now may replace the current one.
   sameDisplays tells whether the displays now equal those of the current
   rolling snapshot. A capture under other displays could have run after
   Windows moved the windows but before the display change reached Lumos, so
   it is refused until a snapshot has been discarded (rollReset), which means
   the new displays have become the normal ones. Call Layout_OnRolled after
   each accepted capture. */
int  Layout_RollAccept(const LayoutState *st, int rollValid, int sameDisplays);
void Layout_OnRolled(LayoutState *st);

#endif /* LAYOUT_H */
