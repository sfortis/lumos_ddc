#include "winpos.h"
#include <string.h>

static WinposLogFn g_log;   /* set only in the debug build */
#define WLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

void Winpos_SetLog(WinposLogFn fn)
{
    g_log = fn;
}

static void LogWindow(const char *tag, const LayoutWindow *w)
{
    WLOG("windows:   %s %ls pid=%lu show=%d normal=%d,%d,%d,%d actual=%d,%d,%d,%d",
         tag, w->cls, w->pid, w->show,
         w->normal.left, w->normal.top, w->normal.right, w->normal.bottom,
         w->actual.left, w->actual.top, w->actual.right, w->actual.bottom);
}

/* Coordinates of other applications' windows are only exact under a
   per-monitor V2 context. Lumos is not DPI aware as a whole, so the context
   is set on the thread around each call and set back afterwards. */
typedef struct { DPI_AWARENESS_CONTEXT prev; } DpiScope;

static void DpiEnter(DpiScope *s)
{
    s->prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

static void DpiLeave(DpiScope *s)
{
    if (s->prev) SetThreadDpiAwarenessContext(s->prev);
}

static LayoutRect FromRect(const RECT *r)
{
    LayoutRect x = { r->left, r->top, r->right, r->bottom };
    return x;
}

static RECT ToRect(const LayoutRect *r)
{
    RECT x = { r->left, r->top, r->right, r->bottom };
    return x;
}

/* ---- Displays ---- */

static BOOL CALLBACK MonitorProc(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp)
{
    (void)hdc; (void)rc;
    LayoutDisplays *out = (LayoutDisplays *)lp;
    if (out->count >= LAYOUT_MAX_MONITORS) return FALSE;

    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) return TRUE;

    LayoutMonitor *m = &out->mon[out->count++];
    m->rect = FromRect(&mi.rcMonitor);
    /* The monitor attached to this adapter output, by device interface path.
       The GDI name is only a fallback: it can change across a drop. */
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0])
        wcsncpy(m->id, dd.DeviceID, LAYOUT_ID_LEN - 1);
    else
        wcsncpy(m->id, mi.szDevice, LAYOUT_ID_LEN - 1);
    m->id[LAYOUT_ID_LEN - 1] = 0;
    return TRUE;
}

static void ReadDisplays(LayoutDisplays *out)
{
    memset(out, 0, sizeof(*out));
    EnumDisplayMonitors(NULL, NULL, MonitorProc, (LPARAM)out);
}

void Winpos_ReadDisplays(LayoutDisplays *out)
{
    DpiScope s;
    DpiEnter(&s);
    ReadDisplays(out);
    DpiLeave(&s);
}

/* ---- Windows ---- */

/* The current state of one window, in the form layout.c compares. */
static BOOL ReadWindow(HWND h, LayoutWindow *w, WINDOWPLACEMENT *wpOut)
{
    WINDOWPLACEMENT wp;
    RECT rc;
    DWORD pid = 0;

    wp.length = sizeof(wp);
    if (!GetWindowPlacement(h, &wp) || !GetWindowRect(h, &rc)) return FALSE;
    GetWindowThreadProcessId(h, &pid);

    memset(w, 0, sizeof(*w));
    w->hwnd = (uintptr_t)h;
    w->pid = pid;
    GetClassNameW(h, w->cls, LAYOUT_CLASS_LEN);
    w->show = IsIconic(h) ? LAYOUT_SHOW_MINIMIZED
            : IsZoomed(h) ? LAYOUT_SHOW_MAXIMIZED
            : LAYOUT_SHOW_NORMAL;
    /* Only a minimized window can restore to maximized; the flag is
       meaningless otherwise and would make an unchanged window look changed. */
    w->restoreMax = w->show == LAYOUT_SHOW_MINIMIZED &&
                    (wp.flags & WPF_RESTORETOMAXIMIZED) != 0;
    w->visible = IsWindowVisible(h) != 0;
    w->normal = FromRect(&wp.rcNormalPosition);
    w->actual = FromRect(&rc);
    if (wpOut) *wpOut = wp;
    return TRUE;
}

static BOOL IsShellClass(const WCHAR *cls)
{
    /* The taskbars and the desktop. CoreWindow is the content of a UWP app;
       its frame (ApplicationFrameWindow) is the window that gets placed. */
    static const WCHAR *kShell[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
        L"Windows.UI.Core.CoreWindow",
    };
    for (size_t i = 0; i < sizeof(kShell) / sizeof(kShell[0]); i++)
        if (wcscmp(cls, kShell[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL Qualifies(HWND h)
{
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return FALSE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return FALSE;
    WCHAR cls[LAYOUT_CLASS_LEN];
    if (!GetClassNameW(h, cls, LAYOUT_CLASS_LEN)) return FALSE;
    return !IsShellClass(cls);
}

static BOOL CALLBACK CaptureProc(HWND h, LPARAM lp)
{
    LayoutSnapshot *snap = (LayoutSnapshot *)lp;
    if (snap->count >= LAYOUT_MAX_WINDOWS) return FALSE;
    /* Cloaked windows (other virtual desktops) are kept: Windows moves them too. */
    if (Qualifies(h) && ReadWindow(h, &snap->win[snap->count], NULL))
        snap->count++;
    return TRUE;
}

void Winpos_Capture(LayoutSnapshot *snap)
{
    DpiScope s;
    DpiEnter(&s);
    snap->count = 0;
    ReadDisplays(&snap->displays);
    EnumWindows(CaptureProc, (LPARAM)snap);
    DpiLeave(&s);
}

/* ---- Windows the user touched ---- */

/* Two clients share the hooks. Tracking records the windows the user touched
   while a snapshot waits for its restore. Watching tells lumos.c that the user
   finished moving, sizing or minimizing a window, so that the rolling snapshot
   is refreshed soon after. Out of context, so nothing is injected into other
   processes; events arrive through this thread's message loop. */
static HWINEVENTHOOK g_hookMove, g_hookMin, g_hookLoc;
static BOOL g_tracking, g_watching;
static HWND g_notifyHwnd;
static UINT g_notifyMsg;
static HWND g_touched[LAYOUT_MAX_WINDOWS];
static int  g_touchedCount;

static void Touch(HWND h, DWORD ev)
{
    if (g_log) {
        WCHAR cls[LAYOUT_CLASS_LEN] = L"?";
        GetClassNameW(h, cls, LAYOUT_CLASS_LEN);
        WLOG("windows: touched %ls by event 0x%04lx", cls, ev);
    }
    for (int i = 0; i < g_touchedCount; i++)
        if (g_touched[i] == h) return;
    if (g_touchedCount < LAYOUT_MAX_WINDOWS)
        g_touched[g_touchedCount++] = h;
}

static unsigned long MsSinceInput(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof(li);
    if (!GetLastInputInfo(&li)) return (unsigned long)-1;
    return GetTickCount() - li.dwTime;
}

/* Records the window as touched when layout.c says the event came from the user. */
static void MaybeTouch(HWND h, DWORD ev, int kind)
{
    if (Layout_IsUserTouch(kind, h == GetForegroundWindow(), MsSinceInput()))
        Touch(h, ev);
    else if (g_log) {
        WCHAR cls[LAYOUT_CLASS_LEN] = L"?";
        GetClassNameW(h, cls, LAYOUT_CLASS_LEN);
        WLOG("windows: ignored event 0x%04lx on %ls (no recent input)", ev, cls);
    }
}

static void CALLBACK UserChangeProc(HWINEVENTHOOK hook, DWORD ev, HWND h, LONG idObject,
                                    LONG idChild, DWORD thread, DWORD time)
{
    (void)hook; (void)thread; (void)time;
    if (!h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (g_tracking)
        MaybeTouch(h, ev, ev == EVENT_SYSTEM_MOVESIZEEND ? LAYOUT_TOUCH_MOVESIZE
                                                         : LAYOUT_TOUCH_MINIMIZE);
    if (g_watching && g_notifyHwnd) PostMessageW(g_notifyHwnd, g_notifyMsg, 0, 0);
}

/* A maximize, an unmaximize or a Win+Arrow snap raises no move/size or
   minimize event, only a location change. The cost of the rule in
   Layout_IsUserTouch: a foreground window that Windows moves while the mouse
   is moving is not restored. This hook runs only while tracking, because the
   cursor raises the event on every mouse move. */
static void CALLBACK LocationProc(HWINEVENTHOOK hook, DWORD ev, HWND h, LONG idObject,
                                  LONG idChild, DWORD thread, DWORD time)
{
    (void)hook; (void)thread; (void)time;
    if (!h || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    /* Cheap checks first: this fires for every window Windows moves. */
    if (h != GetForegroundWindow() || MsSinceInput() > LAYOUT_TOUCH_INPUT_MS) return;
    Touch(h, ev);
}

static BOOL IsTouched(HWND h)
{
    for (int i = 0; i < g_touchedCount; i++)
        if (g_touched[i] == h) return TRUE;
    return FALSE;
}

static void SetHook(HWINEVENTHOOK *hook, BOOL want, DWORD evMin, DWORD evMax, WINEVENTPROC proc)
{
    if (want && !*hook)
        *hook = SetWinEventHook(evMin, evMax, NULL, proc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    else if (!want && *hook) {
        UnhookWinEvent(*hook);
        *hook = NULL;
    }
}

static void UpdateHooks(void)
{
    BOOL any = g_tracking || g_watching;
    SetHook(&g_hookMove, any, EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, UserChangeProc);
    SetHook(&g_hookMin, any, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, UserChangeProc);
    SetHook(&g_hookLoc, g_tracking, EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            LocationProc);
}

void Winpos_Watch(HWND notify, UINT msg)
{
    g_notifyHwnd = notify;
    g_notifyMsg = msg;
    g_watching = TRUE;
    UpdateHooks();
}

void Winpos_Unwatch(void)
{
    g_watching = FALSE;
    UpdateHooks();
}

void Winpos_StartTracking(void)
{
    /* The touched set is not cleared here. A snapshot can wait through
       several unlocks, and a window arranged after the first one stays the
       user's; Winpos_StopTracking clears the set with the snapshot. */
    g_tracking = TRUE;
    UpdateHooks();
}

void Winpos_StopTracking(void)
{
    g_tracking = FALSE;
    g_touchedCount = 0;
    UpdateHooks();
}

/* ---- Restore ---- */

/* How long a window that is not marked hung may take to answer the ping
   before the restore gives up on it. */
#define WINPOS_PING_MS 200

static BOOL SameRect(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

/* Puts one window back. Returns FALSE when Windows refused, which happens for
   the windows of an elevated process. */
static BOOL ApplyOne(HWND h, const LayoutWindow *saved, const WINDOWPLACEMENT *cur)
{
    WINDOWPLACEMENT wp = *cur;
    wp.length = sizeof(wp);
    wp.rcNormalPosition = ToRect(&saved->normal);
    wp.flags = 0;

    if (saved->show == LAYOUT_SHOW_MINIMIZED) {
        if (saved->restoreMax) wp.flags = WPF_RESTORETOMAXIMIZED;
        wp.showCmd = SW_SHOWMINNOACTIVE;
        return SetWindowPlacement(h, &wp);
    }

    if (saved->show == LAYOUT_SHOW_MAXIMIZED) {
        RECT want = ToRect(&saved->actual), now;
        if (IsZoomed(h) && GetWindowRect(h, &now) &&
            MonitorFromRect(&now, MONITOR_DEFAULTTONEAREST) ==
            MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST)) {
            /* SW_SHOWNA keeps the current state and does not activate, so a
               window that is already maximized only gets its normal rect back. */
            wp.showCmd = SW_SHOWNA;
            return SetWindowPlacement(h, &wp);
        }
        /* Not maximized, or maximized on another monitor. Place the normal
           rect first, so that the maximize lands on the monitor of that rect. ShowWindow has no maximize without
           activation; the caller puts the focus back afterwards, and the
           z-order is put back here. */
        HWND above = GetWindow(h, GW_HWNDPREV);
        /* Inserted after a topmost window, the window would become topmost
           itself. Below the topmost band, HWND_TOP is the same position. */
        if (above && (GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
            above = NULL;
        wp.showCmd = SW_SHOWNOACTIVATE;
        if (!SetWindowPlacement(h, &wp)) return FALSE;
        ShowWindow(h, SW_MAXIMIZE);
        SetWindowPos(h, above ? above : HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return TRUE;
    }

    wp.showCmd = SW_SHOWNOACTIVATE;
    if (!SetWindowPlacement(h, &wp)) return FALSE;
    /* A snapped window sits away from its normal rect. Compared after the
       placement, because the normal rect is in workspace coordinates and the
       window rect in screen coordinates. */
    RECT now, want = ToRect(&saved->actual);
    if (GetWindowRect(h, &now) && !SameRect(&now, &want))
        return SetWindowPos(h, NULL, want.left, want.top,
                            want.right - want.left, want.bottom - want.top,
                            SWP_NOZORDER | SWP_NOACTIVATE);
    return TRUE;
}

void Winpos_Restore(const LayoutSnapshot *snap, WinposReport *rep)
{
    DpiScope s;
    DpiEnter(&s);
    memset(rep, 0, sizeof(*rep));
    HWND fg = GetForegroundWindow();

    for (int i = 0; i < snap->count; i++) {
        const LayoutWindow *saved = &snap->win[i];
        HWND h = (HWND)saved->hwnd;
        LayoutWindow now;
        WINDOWPLACEMENT cur;
        BOOL exists = IsWindow(h) && ReadWindow(h, &now, &cur);
        DWORD_PTR ignored;

        int decision = Layout_Decide(saved, exists ? &now : NULL, IsTouched(h));
        static const char *kDecision[] = { "restore", "gone", "touched", "unchanged", "hidden" };
        LogWindow(kDecision[decision], saved);
        if (exists && decision != LAYOUT_SKIP_UNCHANGED) LogWindow("  now", &now);

        switch (decision) {
        case LAYOUT_SKIP_GONE:      rep->gone++;      break;
        case LAYOUT_SKIP_TOUCHED:   rep->touched++;   break;
        case LAYOUT_SKIP_UNCHANGED: rep->unchanged++; break;
        case LAYOUT_SKIP_HIDDEN:    rep->hidden++;    break;
        default:
            /* The placement calls wait for the window's thread. A hung
               application would freeze this thread, and with it the mouse
               hook that shares it, so a window that does not answer a ping
               at once is left alone. */
            if (!SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                     WINPOS_PING_MS, &ignored))
                rep->hung++;
            else if (ApplyOne(h, saved, &cur)) rep->restored++;
            else                          rep->failed++;
            break;
        }
    }

    /* Maximizing activates the window. Give the focus back to the window
       that had it. */
    if (fg && GetForegroundWindow() != fg && IsWindow(fg))
        SetForegroundWindow(fg);
    DpiLeave(&s);
}

/* ---- Diagnostics ---- */

void Winpos_LogSnapshot(const LayoutSnapshot *snap, const char *what)
{
    if (!g_log) return;
    WLOG("windows: %s holds %d windows", what, snap->count);
    for (int i = 0; i < snap->count; i++)
        LogWindow("saved", &snap->win[i]);
}

/* Logs every saved window that is no longer where the snapshot has it. Run a
   few seconds after a restore, it shows an application that resized itself
   afterwards, for example on the DPI change of the returning monitor. */
void Winpos_Verify(const LayoutSnapshot *snap)
{
    if (!g_log) return;
    DpiScope s;
    DpiEnter(&s);
    int moved = 0;
    for (int i = 0; i < snap->count; i++) {
        const LayoutWindow *saved = &snap->win[i];
        LayoutWindow now;
        if (!IsWindow((HWND)saved->hwnd) || !ReadWindow((HWND)saved->hwnd, &now, NULL))
            continue;
        if (Layout_Decide(saved, &now, 0) != LAYOUT_RESTORE)
            continue;
        LogWindow("differs", saved);
        LogWindow("    now", &now);
        moved++;
    }
    WLOG("windows: verify after restore: %d window(s) differ from the snapshot", moved);
    DpiLeave(&s);
}
