#include "layout.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

#define TRACK_QUIET_EXPIRY (LAYOUT_DO_TRACK | LAYOUT_DO_QUIET | LAYOUT_DO_EXPIRY)
#define RESTORE_DISCARD    (LAYOUT_DO_RESTORE | LAYOUT_DO_DISCARD)

static LayoutRect R(int l, int t, int r, int b)
{
    LayoutRect x = { l, t, r, b };
    return x;
}

static void AddMonitor(LayoutDisplays *d, const wchar_t *id, LayoutRect rc)
{
    LayoutMonitor *m = &d->mon[d->count++];
    wcsncpy(m->id, id, LAYOUT_ID_LEN - 1);
    m->id[LAYOUT_ID_LEN - 1] = 0;
    m->rect = rc;
}

static LayoutWindow Win(int show, LayoutRect normal, LayoutRect actual)
{
    LayoutWindow w;
    memset(&w, 0, sizeof(w));
    w.hwnd = 0x1234;
    w.pid = 42;
    wcscpy(w.cls, L"Notepad");
    w.show = show;
    w.visible = 1;
    w.normal = normal;
    w.actual = actual;
    return w;
}

static void TestDisplays(void)
{
    LayoutDisplays a = { 0 }, b = { 0 };
    AddMonitor(&a, L"\\\\?\\DISPLAY#SAM7868#UID4161", R(0, 0, 5120, 1440));
    AddMonitor(&a, L"\\\\?\\DISPLAY#DEL1234#UID1", R(5120, 0, 7680, 1440));
    /* the same monitors, enumerated in the other order */
    AddMonitor(&b, L"\\\\?\\DISPLAY#DEL1234#UID1", R(5120, 0, 7680, 1440));
    AddMonitor(&b, L"\\\\?\\DISPLAY#SAM7868#UID4161", R(0, 0, 5120, 1440));
    CHECK(Layout_SameDisplays(&a, &b), "same monitors in another order");

    LayoutDisplays c = b;
    wcscpy(c.mon[0].id, L"\\\\?\\DISPLAY#DEL1234#UID2");
    CHECK(!Layout_SameDisplays(&a, &c), "another monitor id");

    LayoutDisplays d = b;
    d.mon[1].rect = R(0, 0, 1920, 1080);
    CHECK(!Layout_SameDisplays(&a, &d), "another rect");

    LayoutDisplays e = b;
    e.count = 1;
    CHECK(!Layout_SameDisplays(&a, &e), "another count");
}

static void TestDecide(void)
{
    LayoutWindow saved = Win(LAYOUT_SHOW_NORMAL, R(10, 10, 800, 600), R(10, 10, 800, 600));
    LayoutWindow now = saved;

    CHECK(Layout_Decide(&saved, NULL, 0) == LAYOUT_SKIP_GONE, "closed window");
    now.pid = 43;
    CHECK(Layout_Decide(&saved, &now, 0) == LAYOUT_SKIP_GONE, "handle reused by another process");
    now = saved;
    wcscpy(now.cls, L"Chrome_WidgetWin_1");
    CHECK(Layout_Decide(&saved, &now, 0) == LAYOUT_SKIP_GONE, "handle reused by another class");

    now = saved;
    now.normal = R(0, 0, 640, 480);
    CHECK(Layout_Decide(&saved, &now, 1) == LAYOUT_SKIP_TOUCHED, "touched by the user");
    CHECK(Layout_Decide(&saved, &now, 0) == LAYOUT_RESTORE, "moved by Windows");

    now = saved;
    CHECK(Layout_Decide(&saved, &now, 0) == LAYOUT_SKIP_UNCHANGED, "unchanged");

    /* An application hid its window after the snapshot (a password manager
       that went to the tray). The restore must never show it again. */
    now = saved;
    now.visible = 0;
    now.normal = R(0, 0, 640, 480);
    CHECK(Layout_Decide(&saved, &now, 0) == LAYOUT_SKIP_HIDDEN, "hidden since the snapshot");

    /* A snapped window: same normal rect, the snapped rect differs */
    LayoutWindow snapped = Win(LAYOUT_SHOW_NORMAL, R(10, 10, 800, 600), R(0, 0, 2560, 1400));
    now = snapped;
    now.actual = R(0, 0, 640, 480);
    CHECK(Layout_Decide(&snapped, &now, 0) == LAYOUT_RESTORE, "snapped rect lost");

    /* A maximized window that Windows moved to another monitor keeps its
       state and its normal rect; only the actual rect shows the move. */
    LayoutWindow max = Win(LAYOUT_SHOW_MAXIMIZED, R(10, 10, 800, 600), R(-8, -8, 5128, 1400));
    now = max;
    now.actual = R(5112, -8, 7688, 1400);
    CHECK(Layout_Decide(&max, &now, 0) == LAYOUT_RESTORE, "maximized on another monitor");
    now = max;
    CHECK(Layout_Decide(&max, &now, 0) == LAYOUT_SKIP_UNCHANGED, "maximized, unchanged");
    now = max;
    now.show = LAYOUT_SHOW_NORMAL;
    CHECK(Layout_Decide(&max, &now, 0) == LAYOUT_RESTORE, "maximized lost");
}

static void TestLockPath(void)
{
    LayoutState st;

    /* A lock without a drop discards the snapshot after the quiet period */
    Layout_Init(&st);
    CHECK(Layout_OnLock(&st) == LAYOUT_DO_SNAPSHOT, "lock takes a snapshot");
    CHECK(Layout_OnUnlock(&st) == TRACK_QUIET_EXPIRY, "unlock starts the usable period");
    CHECK(Layout_OnQuiet(&st, 1) == LAYOUT_DO_DISCARD, "no drop, no restore");

    /* One drop during the lock */
    Layout_Init(&st);
    Layout_OnLock(&st);
    CHECK(Layout_OnDisplayChange(&st, 0) == 0, "change while locked");
    CHECK(Layout_OnUnlock(&st) == TRACK_QUIET_EXPIRY, "unlock after a drop");
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore after one drop");

    /* Four drops, as on 2026-11-06 */
    Layout_Init(&st);
    Layout_OnLock(&st);
    for (int i = 0; i < 8; i++) Layout_OnDisplayChange(&st, 0);
    Layout_OnUnlock(&st);
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore after four drops");

    /* The display turns on at the lock screen and drops only then */
    Layout_Init(&st);
    Layout_OnLock(&st);
    CHECK(Layout_OnDisplayOff(&st) == 0, "display off after the lock keeps the snapshot");
    CHECK(Layout_OnDisplayOn(&st) == 0, "display on while locked is ignored");
    Layout_OnDisplayChange(&st, 0);
    Layout_OnUnlock(&st);
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "drop at wake is restored");

    /* The layout never returns, the snapshot expires */
    Layout_Init(&st);
    Layout_OnLock(&st);
    Layout_OnDisplayChange(&st, 0);
    Layout_OnUnlock(&st);
    CHECK(Layout_OnQuiet(&st, 0) == 0, "other displays: wait");
    CHECK(Layout_OnExpiry(&st) == LAYOUT_DO_DISCARD, "expiry discards");
    CHECK(Layout_OnQuiet(&st, 1) == 0, "nothing left after expiry");

    /* The feature is turned off while a snapshot waits */
    Layout_Init(&st);
    Layout_OnLock(&st);
    Layout_OnDisplayChange(&st, 0);
    CHECK(Layout_OnDisable(&st) == LAYOUT_DO_DISCARD, "disable discards");
    CHECK(Layout_OnDisable(&st) == 0, "disable twice");

    /* A second lock while a snapshot waits does not replace it */
    Layout_Init(&st);
    Layout_OnLock(&st);
    Layout_OnDisplayChange(&st, 0);
    Layout_OnUnlock(&st);
    CHECK(Layout_OnLock(&st) == 0, "second lock keeps the snapshot");
    CHECK(Layout_OnQuiet(&st, 1) == 0, "no restore while locked again");
    CHECK(Layout_OnUnlock(&st) == TRACK_QUIET_EXPIRY, "second unlock");
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore after the second unlock");

    /* The expiry timer of the first usable period fires during a second lock.
       The snapshot must survive it; the next unlock starts a new expiry. */
    Layout_Init(&st);
    Layout_OnLock(&st);
    Layout_OnDisplayChange(&st, 0);
    Layout_OnUnlock(&st);
    CHECK(Layout_OnQuiet(&st, 0) == 0, "displays still away after the unlock");
    Layout_OnLock(&st);
    CHECK(Layout_OnExpiry(&st) == 0, "expiry while locked again is ignored");
    Layout_OnDisplayChange(&st, 0);
    CHECK(Layout_OnUnlock(&st) == TRACK_QUIET_EXPIRY, "unlock restarts the expiry");
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore after the expiry was ignored");

    /* The same with the display turned off instead of a lock */
    Layout_Init(&st);
    Layout_OnDisplayOff(&st);
    Layout_OnDisplayChange(&st, 0);
    Layout_OnDisplayOn(&st);
    Layout_OnDisplayOff(&st);
    CHECK(Layout_OnExpiry(&st) == 0, "expiry while the display is off again is ignored");
}

static void TestDisplayOffPath(void)
{
    LayoutState st;

    /* Drops while the display is off: no restore before display-on */
    Layout_Init(&st);
    CHECK(Layout_OnDisplayOff(&st) == LAYOUT_DO_SNAPSHOT, "display off takes a snapshot");
    CHECK(Layout_OnDisplayChange(&st, 0) == 0, "change while off");
    Layout_OnDisplayChange(&st, 0);
    CHECK(Layout_OnQuiet(&st, 1) == 0, "no restore while the display is off");
    CHECK(Layout_OnDisplayOn(&st) == TRACK_QUIET_EXPIRY, "display on starts the usable period");
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore after display on");

    /* A drop at wake, after display-on */
    Layout_Init(&st);
    Layout_OnDisplayOff(&st);
    Layout_OnDisplayOn(&st);
    CHECK(Layout_OnDisplayChange(&st, 0) == (LAYOUT_DO_QUIET | LAYOUT_DO_EXPIRY),
          "change while usable restarts quiet and expiry");
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore after a drop at wake");

    /* After a restore the next display-off takes a new snapshot */
    CHECK(Layout_OnDisplayOff(&st) == LAYOUT_DO_SNAPSHOT, "new snapshot after a restore");

    /* A display-on without a snapshot does nothing */
    Layout_Init(&st);
    CHECK(Layout_OnDisplayOn(&st) == 0, "display on without a snapshot");
    CHECK(Layout_OnDisplayChange(&st, 0) == 0, "change without a snapshot");
}

static void TestRollingPath(void)
{
    LayoutState st;

    /* The monitor is switched off at its button: no lock, no display-off.
       The rolling snapshot from before the drop is adopted at once. */
    Layout_Init(&st);
    CHECK(Layout_CanRefresh(&st), "idle: the rolling snapshot may refresh");
    CHECK(Layout_OnDisplayChange(&st, 1) == (LAYOUT_DO_ADOPT | TRACK_QUIET_EXPIRY),
          "drop without a lock adopts the rolling snapshot");
    CHECK(!Layout_CanRefresh(&st), "no refresh while a drop is pending");
    CHECK(Layout_OnDisplayChange(&st, 1) == (LAYOUT_DO_QUIET | LAYOUT_DO_EXPIRY),
          "further changes restart quiet and expiry");
    CHECK(Layout_OnQuiet(&st, 0) == 0, "placeholder still there: wait");
    CHECK(Layout_OnQuiet(&st, 1) == RESTORE_DISCARD, "restore when the monitor returns");
    CHECK(Layout_CanRefresh(&st), "refresh again after the restore");

    /* Without a rolling snapshot nothing can be restored */
    Layout_Init(&st);
    CHECK(Layout_OnDisplayChange(&st, 0) == 0, "drop without any snapshot");

    /* No refresh while locked or while the display is off */
    Layout_Init(&st);
    Layout_OnLock(&st);
    CHECK(!Layout_CanRefresh(&st), "no refresh while locked");
    Layout_OnUnlock(&st);
    Layout_OnQuiet(&st, 1);   /* undisturbed: discarded */
    CHECK(Layout_CanRefresh(&st), "refresh after the lock snapshot is discarded");
    Layout_OnDisplayOff(&st);
    CHECK(!Layout_CanRefresh(&st), "no refresh while the display is off");
    Layout_OnDisplayOn(&st);
    Layout_OnQuiet(&st, 1);
    CHECK(Layout_CanRefresh(&st), "refresh after display on and discard");

    /* A drop while the display is off uses the display-off snapshot, not
       the rolling one */
    Layout_Init(&st);
    Layout_OnDisplayOff(&st);
    CHECK(Layout_OnDisplayChange(&st, 1) == 0, "drop while off keeps the display-off snapshot");
}

static void TestRollingAccept(void)
{
    LayoutState st;
    Layout_Init(&st);

    /* The first capture is always accepted */
    CHECK(Layout_RollAccept(&st, 0, 0), "first capture");
    Layout_OnRolled(&st);
    CHECK(Layout_RollAccept(&st, 1, 1), "same displays");
    /* A capture under other displays may be taken after Windows moved the
       windows but before the display change reached Lumos. */
    CHECK(!Layout_RollAccept(&st, 1, 0), "other displays before the change was handled");

    /* After a snapshot was discarded (a new monitor that stays, for
       example), the new displays are the normal ones. */
    Layout_OnDisplayChange(&st, 1);
    Layout_OnQuiet(&st, 0);
    Layout_OnExpiry(&st);
    CHECK(Layout_RollAccept(&st, 1, 0), "other displays after the discard");
    Layout_OnRolled(&st);
    CHECK(!Layout_RollAccept(&st, 1, 0), "and only until the next capture");

    /* Never while a snapshot is pending */
    Layout_OnLock(&st);
    CHECK(!Layout_RollAccept(&st, 1, 1), "not while locked");
}

static void TestUserTouch(void)
{
    /* On 2026-11-06 Windows raised MINIMIZEEND for Firefox 170 ms after a
       drop, with no input from the user. It must not count as a touch. */
    CHECK(!Layout_IsUserTouch(LAYOUT_TOUCH_MINIMIZE, 0, 60000), "minimize event without input");
    CHECK(Layout_IsUserTouch(LAYOUT_TOUCH_MINIMIZE, 0, 200), "minimize right after a click");
    CHECK(!Layout_IsUserTouch(LAYOUT_TOUCH_MOVESIZE, 1, 5000), "move/size end without recent input");
    CHECK(Layout_IsUserTouch(LAYOUT_TOUCH_MOVESIZE, 0, 50), "move/size end after a drag");
    CHECK(Layout_IsUserTouch(LAYOUT_TOUCH_LOCATION, 1, 300), "location change of the foreground window");
    CHECK(!Layout_IsUserTouch(LAYOUT_TOUCH_LOCATION, 0, 300), "location change of a background window");
    CHECK(!Layout_IsUserTouch(LAYOUT_TOUCH_LOCATION, 1, 1500), "location change long after input");
}

int main(void)
{
    TestDisplays();
    TestDecide();
    TestLockPath();
    TestDisplayOffPath();
    TestRollingPath();
    TestRollingAccept();
    TestUserTouch();
    if (failures) {
        printf("test_layout: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_layout: all passed\n");
    return 0;
}
