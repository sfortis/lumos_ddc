#include "layout.h"
#include <string.h>

static int SameRect(const LayoutRect *a, const LayoutRect *b)
{
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

int Layout_SameDisplays(const LayoutDisplays *a, const LayoutDisplays *b)
{
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; i++) {
        int found = 0;
        for (int j = 0; j < b->count && !found; j++)
            found = wcscmp(a->mon[i].id, b->mon[j].id) == 0 &&
                    SameRect(&a->mon[i].rect, &b->mon[j].rect);
        if (!found) return 0;
    }
    return 1;
}

int Layout_Decide(const LayoutWindow *saved, const LayoutWindow *now, int touched)
{
    /* Handles are reused, so a handle that now belongs to another process or
       class is an unrelated window and must not be moved. */
    if (!now || now->pid != saved->pid || wcscmp(now->cls, saved->cls) != 0)
        return LAYOUT_SKIP_GONE;
    /* The placement calls show a window. One the application hid since the
       snapshot (a password manager in the tray) must stay hidden. */
    if (!now->visible)
        return LAYOUT_SKIP_HIDDEN;
    if (touched)
        return LAYOUT_SKIP_TOUCHED;
    if (now->show == saved->show && SameRect(&now->normal, &saved->normal) &&
        now->restoreMax == saved->restoreMax &&
        /* The actual rect shows a snapped window that lost its snap, and a
           maximized window that Windows moved to another monitor while it
           kept its state. A minimized window has no meaningful one. */
        (saved->show == LAYOUT_SHOW_MINIMIZED || SameRect(&now->actual, &saved->actual)))
        return LAYOUT_SKIP_UNCHANGED;
    return LAYOUT_RESTORE;
}

int Layout_IsUserTouch(int kind, int isForeground, unsigned long msSinceInput)
{
    if (msSinceInput > LAYOUT_TOUCH_INPUT_MS)
        return 0;
    return kind != LAYOUT_TOUCH_LOCATION || isForeground;
}

void Layout_Init(LayoutState *st)
{
    memset(st, 0, sizeof(*st));
}

static int Disarm(LayoutState *st)
{
    st->armed = st->disturbed = st->usable = 0;
    st->rollReset = 1;
    return LAYOUT_DO_DISCARD;
}

/* The session can be used again: tracking, the quiet period and the expiry
   all start now. */
static int BecomeUsable(LayoutState *st)
{
    st->usable = 1;
    return LAYOUT_DO_TRACK | LAYOUT_DO_QUIET | LAYOUT_DO_EXPIRY;
}

int Layout_OnLock(LayoutState *st)
{
    st->locked = 1;
    st->usable = 0;
    if (st->armed)
        return 0;   /* the first snapshot is the good one; later ones may hold the placeholder */
    st->armed = 1;
    st->disturbed = 0;
    return LAYOUT_DO_SNAPSHOT;
}

int Layout_OnUnlock(LayoutState *st)
{
    st->locked = 0;
    return st->armed ? BecomeUsable(st) : 0;
}

int Layout_OnDisplayOff(LayoutState *st)
{
    st->usable = 0;
    st->displayOff = 1;
    if (st->armed)
        return 0;
    st->armed = 1;
    st->disturbed = 0;
    return LAYOUT_DO_SNAPSHOT;
}

int Layout_OnDisplayOn(LayoutState *st)
{
    st->displayOff = 0;
    /* At the lock screen the unlock decides; the display can still drop at
       wake, before the user is in. */
    if (st->locked || !st->armed || st->usable)
        return 0;
    return BecomeUsable(st);
}

int Layout_OnDisplayChange(LayoutState *st, int haveRolling)
{
    if (!st->armed) {
        /* Not armed means unlocked with the display on, because a lock and
           a display-off both take a snapshot. Windows has already moved the
           windows when this arrives, so only the rolling snapshot, taken
           before the drop, describes the arrangement. */
        if (!haveRolling)
            return 0;
        st->armed = 1;
        st->disturbed = 1;
        return LAYOUT_DO_ADOPT | BecomeUsable(st);
    }
    /* Any change counts, not only one that shows the placeholder: windows
       that did not move are skipped as unchanged, so the restore costs
       nothing then. */
    st->disturbed = 1;
    /* The expiry counts from the last change: a monitor switched off at its
       button for a long break must still be restored when it returns. */
    return st->usable ? LAYOUT_DO_QUIET | LAYOUT_DO_EXPIRY : 0;
}

int Layout_OnQuiet(LayoutState *st, int sameDisplays)
{
    if (!st->armed || !st->usable)
        return 0;
    if (!st->disturbed)
        return Disarm(st);
    if (!sameDisplays)
        return 0;   /* wait for the displays to return, or for the expiry */
    return LAYOUT_DO_RESTORE | Disarm(st);
}

int Layout_OnExpiry(LayoutState *st)
{
    /* The timer of an earlier usable period can fire during a later lock or
       display-off. The snapshot is still waiting then, and the next usable
       period starts a new expiry. */
    return st->armed && st->usable ? Disarm(st) : 0;
}

int Layout_OnDisable(LayoutState *st)
{
    return st->armed ? Disarm(st) : 0;
}

int Layout_CanRefresh(const LayoutState *st)
{
    return !st->armed && !st->locked && !st->displayOff;
}

int Layout_RollAccept(const LayoutState *st, int rollValid, int sameDisplays)
{
    return Layout_CanRefresh(st) && (!rollValid || sameDisplays || st->rollReset);
}

void Layout_OnRolled(LayoutState *st)
{
    st->rollReset = 0;
}
