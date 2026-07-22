#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "resource.h"
#include "monitor.h"
#include "ui.h"
#include "presets.h"

/* GUID_CONSOLE_DISPLAY_STATE {6FE69556-704A-47A0-8F24-C28D936FDA47}
   Defined manually because some MinGW headers omit it. Fires on display
   power on/off (including the transition back on after a lock screen). */
static const GUID kGuidConsoleDisplayState =
    { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

/* Coalesce the near-simultaneous unlock + display-power triggers into one
   re-enumeration so we don't hammer the DDC/I2C bus. */
#define RESCAN_TIMER_ID     0xB100
#define RESCAN_DEBOUNCE_MS  600

/* Posted by the rescan worker thread when the fresh MonitorList is ready.
   lParam = MonitorList* (heap, adopted and freed by the main thread). */
#define WM_APP_RESCAN_DONE  (WM_APP + 1)

/* Schedule tick: recompute the interpolated brightness once a minute. */
#define SCHEDULE_TIMER_ID   0xB101
#define SCHEDULE_TICK_MS    60000

static HINSTANCE    g_hInst;
static HWND         g_hwndHidden;    /* Hidden top-level window (receives broadcasts + notifications) */
static HWND         g_hwndPopup;
static MonitorList  g_monitors;
static Settings     g_settings;
static NOTIFYICONDATAW g_nid;
static HHOOK        g_mouseHook;
static HPOWERNOTIFY g_hPowerNotify;   /* GUID_CONSOLE_DISPLAY_STATE registration */
static UINT         g_wmTakeover;     /* cross-process "quit, I'm replacing you" message */
static volatile LONG g_rescanBusy;    /* 1 while a rescan worker thread is in flight */
static BOOL         g_rescanPending;  /* a trigger arrived mid-rescan; run once more (main thread only) */
static BOOL         g_scheduleSuspended = FALSE;
static int          g_scheduleSuspendMinute = 0;   /* minute-of-day at suspend */
static int          g_scheduleResumeMinute = 0;    /* next anchor to resume at */
static int          g_scheduleLastApplied = -1;    /* last brightness pushed by the schedule */

static const WCHAR APPCLASS[] = L"LumosMain";

/* Forward declarations */
static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
static void CreateTrayIcon(HWND hwnd);
static void RemoveTrayIcon(void);
static void RegisterHotkeys(HWND hwnd);
static void UnregisterHotkeys(HWND hwnd);
static void ShowContextMenu(HWND hwnd);
static void HandleHotkey(int id);
static void ApplyPreset(int index);
static void InstallMouseHook(void);
static void RemoveMouseHook(void);
static void ScheduleRescan(HWND hwnd);
static void StartRescan(HWND hwnd);
static DWORD WINAPI RescanThreadProc(LPVOID param);
static void Schedule_ApplyNow(void);
static void Schedule_Suspend(void);

static void SaveDeltasCallback(void)
{
    Settings_SaveDeltas(&g_settings, &g_monitors);
    Settings_Save(&g_settings);
}

/* ---- Entry Point ---- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLineA, int showCmd)
{
    (void)hPrev; (void)cmdLineA; (void)showCmd;
    g_hInst = hInst;

    /* Unique cross-process message id (same value in every Lumos build).
       Used both to signal an older instance to quit and to receive that signal. */
    g_wmTakeover = RegisterWindowMessageW(L"Lumos_TakeoverQuit");

    /* Single-instance with clean handoff: request initial ownership so the mutex
       stays non-signaled while we run. If it already exists, another instance is
       live: ask it to quit, then wait (bounded) to take over. WAIT_ABANDONED means
       the old owner died holding it; both that and WAIT_OBJECT_0 mean we acquired. */
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Lumos_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_wmTakeover, 0, 0);
        DWORD w = WaitForSingleObject(hMutex, 5000);
        if (w == WAIT_TIMEOUT || w == WAIT_FAILED) {
            /* Old instance did not release in time (likely a pre-handoff build).
               Refuse to run a second copy rather than fight over the DDC bus. */
            CloseHandle(hMutex);
            return 0;
        }
    }

    /* Initialize COM for Shell */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* Initialize common controls */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    /* Init subsystems */
    memset(&g_monitors, 0, sizeof(g_monitors));
    Monitor_Enumerate(&g_monitors);
    Settings_Init(&g_settings);
    Settings_LoadDeltas(&g_settings, &g_monitors);

    if (!UI_Init(hInst)) {
        MessageBoxW(NULL, L"Failed to initialize UI", APP_NAME, MB_ICONERROR);
        return 1;
    }

    /* Register hidden window class */
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APPCLASS;
    RegisterClassExW(&wc);

    /* Create hidden top-level window. Not HWND_MESSAGE: message-only windows
       do not receive broadcast messages like WM_DISPLAYCHANGE. WS_EX_TOOLWINDOW
       keeps it off the taskbar/alt-tab; it is never shown. */
    g_hwndHidden = CreateWindowExW(WS_EX_TOOLWINDOW, APPCLASS, APP_NAME,
                                    WS_POPUP, 0, 0, 0, 0,
                                    NULL, NULL, hInst, NULL);

    /* Re-enumerate on session unlock and on display power-on. Windows does not
       reliably send WM_DISPLAYCHANGE across a lock screen, so cached DDC handles
       go stale; these notifications trigger a rescan to re-acquire them. */
    WTSRegisterSessionNotification(g_hwndHidden, NOTIFY_FOR_THIS_SESSION);
    g_hPowerNotify = RegisterPowerSettingNotification(
        g_hwndHidden, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);

    /* Create popup (hidden) */
    g_hwndPopup = UI_CreatePopup(hInst, &g_monitors);
    UI_SetDeltaSaveCallback(SaveDeltasCallback);

    /* Tray icon, hotkeys, mouse hook */
    CreateTrayIcon(g_hwndHidden);
    RegisterHotkeys(g_hwndHidden);
    InstallMouseHook();

    /* Brightness schedule: suspend on manual slider changes, tick every minute,
       and apply the current time slot immediately at startup. */
    UI_SetManualChangeCallback(Schedule_Suspend);
    SetTimer(g_hwndHidden, SCHEDULE_TIMER_ID, SCHEDULE_TICK_MS, NULL);
    Schedule_ApplyNow();

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Cleanup */
    RemoveMouseHook();
    UnregisterHotkeys(g_hwndHidden);
    if (g_hPowerNotify) UnregisterPowerSettingNotification(g_hPowerNotify);
    WTSUnRegisterSessionNotification(g_hwndHidden);
    RemoveTrayIcon();
    Monitor_Cleanup(&g_monitors);
    UI_Shutdown();
    CoUninitialize();
    /* Release before closing so a replacing instance sees WAIT_OBJECT_0 promptly
       instead of waiting for abandonment. */
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return (int)msg.wParam;
}

/* ---- Tray Icon ---- */

static void CreateTrayIcon(HWND hwnd)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_LUMOS));
    if (!g_nid.hIcon)
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, APP_NAME L" - Monitor Brightness");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* ---- Mouse wheel on tray icon ---- */

#ifdef DEBUG
static FILE *g_dbgLog = NULL;
static void DbgLog(const char *fmt, ...)
{
    if (!g_dbgLog) {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        WCHAR *dot = wcsrchr(path, L'.');
        if (dot) wcscpy(dot, L"_hook.log");
        else wcscat(path, L"_hook.log");
        g_dbgLog = _wfopen(path, L"a");
    }
    if (!g_dbgLog) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_dbgLog, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dbgLog, fmt, ap);
    va_end(ap);
    fprintf(g_dbgLog, "\n");
    fflush(g_dbgLog);
}
#else
#define DbgLog(...) ((void)0)
#endif

static BOOL IsCursorOverTrayIcon(POINT ptPhysical)
{
    NOTIFYICONIDENTIFIER nii;
    memset(&nii, 0, sizeof(nii));
    nii.cbSize = sizeof(nii);
    nii.hWnd = g_nid.hWnd;
    nii.uID = g_nid.uID;
    RECT rcIcon;
    HRESULT hr = Shell_NotifyIconGetRect(&nii, &rcIcon);
    if (SUCCEEDED(hr)) {
        /* Both rcIcon and ptPhysical are in physical (unscaled) pixels */
        BOOL hit = PtInRect(&rcIcon, ptPhysical);
        DbgLog("GetRect OK: icon=[%d,%d,%d,%d] cursor=[%d,%d] hit=%d",
               rcIcon.left, rcIcon.top, rcIcon.right, rcIcon.bottom,
               ptPhysical.x, ptPhysical.y, hit);
        return hit;
    }
    DbgLog("GetRect FAILED hr=0x%08X hwnd=%p id=%u",
           (unsigned)hr, (void*)nii.hWnd, nii.uID);
    return FALSE;
}

static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT *mhs = (MSLLHOOKSTRUCT *)lParam;
        DbgLog("WM_MOUSEWHEEL at [%d,%d] mouseData=0x%08X",
               (int)mhs->pt.x, (int)mhs->pt.y, (unsigned)mhs->mouseData);
        if (IsCursorOverTrayIcon(mhs->pt)) {
            short delta = (short)HIWORD(mhs->mouseData);
            PostMessageW(g_hwndHidden, WM_HOTKEY,
                         (WPARAM)(delta > 0 ? WM_HOTKEY_BRIGHTEN : WM_HOTKEY_DIM), 0);
            return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static void InstallMouseHook(void)
{
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, g_hInst, 0);
    DbgLog("InstallMouseHook: handle=%p err=%u", (void*)g_mouseHook, (unsigned)GetLastError());
}

static void RemoveMouseHook(void)
{
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = NULL;
    }
}

/* ---- Hotkeys ---- */

static void RegisterHotkeys(HWND hwnd)
{
    RegisterHotKey(hwnd, WM_HOTKEY_BRIGHTEN, MOD_CONTROL | MOD_ALT, VK_UP);
    RegisterHotKey(hwnd, WM_HOTKEY_DIM,      MOD_CONTROL | MOD_ALT, VK_DOWN);
}

static void UnregisterHotkeys(HWND hwnd)
{
    UnregisterHotKey(hwnd, WM_HOTKEY_BRIGHTEN);
    UnregisterHotKey(hwnd, WM_HOTKEY_DIM);
}

/* ---- Context Menu ---- */

static void ShowContextMenu(HWND hwnd)
{
    UI_ShowContextMenu(hwnd, &g_settings);
}

/* ---- Hotkey Handler ---- */

static int g_masterTarget = -1;  /* tracks intended base percent across hotkey presses */

static void HandleHotkey(int id)
{
    int step = g_settings.step;
    int delta = 0;

    switch (id) {
    case WM_HOTKEY_BRIGHTEN: delta = step;  break;
    case WM_HOTKEY_DIM:      delta = -step; break;
    default: return;
    }

    /* Initialize target from current state if needed */
    if (g_masterTarget < 0) {
        int sum = 0, cnt = 0;
        for (int i = 0; i < g_monitors.count; i++) {
            BrightMonitor *mon = &g_monitors.monitors[i];
            if (!mon->controllable) continue;
            DWORD range = mon->brightnessMax - mon->brightnessMin;
            int pct = range > 0 ? (int)(((mon->brightnessCur - mon->brightnessMin) * 100) / range) : 0;
            /* Subtract delta to recover the base value */
            sum += pct - mon->delta;
            cnt++;
        }
        g_masterTarget = cnt > 0 ? sum / cnt : 50;
    }

    g_masterTarget += delta;

    /* Allow target to exceed 0-100 so monitors with large deltas can reach full range.
       Limits: every monitor's (target + delta) should be able to span 0-100. */
    int minDelta = 0, maxDelta = 0;
    for (int i = 0; i < g_monitors.count; i++) {
        if (!g_monitors.monitors[i].controllable) continue;
        int d = g_monitors.monitors[i].delta;
        if (d < minDelta) minDelta = d;
        if (d > maxDelta) maxDelta = d;
    }
    int lo = 0 - maxDelta;   /* so monitor with max delta can reach 0 */
    int hi = 100 - minDelta;  /* so monitor with min delta can reach 100 */
    if (g_masterTarget < lo) g_masterTarget = lo;
    if (g_masterTarget > hi) g_masterTarget = hi;

    Monitor_SetAllBrightness(&g_monitors, g_masterTarget);

    /* Show OSD on primary monitor (where cursor is) */
    Monitor_RefreshBrightness(&g_monitors);
    {
        POINT curPos;
        GetCursorPos(&curPos);
        HMONITOR hCurMon = MonitorFromPoint(curPos, MONITOR_DEFAULTTOPRIMARY);
        /* Find matching monitor for percentage display, fallback to first */
        int pct = 50;
        for (int i = 0; i < g_monitors.count; i++) {
            if (g_monitors.monitors[i].hMonitor == hCurMon && g_monitors.monitors[i].controllable) {
                BrightMonitor *mon = &g_monitors.monitors[i];
                DWORD range = mon->brightnessMax - mon->brightnessMin;
                pct = range > 0 ? (int)(((mon->brightnessCur - mon->brightnessMin) * 100) / range) : 0;
                break;
            }
        }
        UI_ShowOSD(g_hInst, hCurMon, pct);
    }

    /* Update popup if visible */
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    Schedule_Suspend();  /* manual change: yield schedule control until next anchor */
}

/* ---- Apply Preset ---- */

static void ApplyPreset(int index)
{
    if (index < 0 || index >= g_settings.presetCount) return;
    g_masterTarget = (int)g_settings.presets[index].brightness;
    Monitor_SetAllBrightness(&g_monitors, g_settings.presets[index].brightness);
    Monitor_RefreshBrightness(&g_monitors);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);

    Schedule_Suspend();  /* manual change: yield schedule control until next anchor */
}

/* ---- Monitor rescan (async) ---- */

/* Worker thread: runs the slow DDC/CI enumeration OFF the UI thread, then hands
   the fresh list back via WM_APP_RESCAN_DONE. Kept off-thread because these
   calls can block for seconds while displays settle after unlock/power-on, and
   blocking the UI thread would also stall the WH_MOUSE_LL hook that lives on it,
   causing system-wide mouse jank. Only handle acquisition happens here; all
   window work stays on the main thread. */
static DWORD WINAPI RescanThreadProc(LPVOID param)
{
    HWND hwnd = (HWND)param;
    MonitorList *fresh = (MonitorList *)calloc(1, sizeof(MonitorList));
    if (fresh)
        Monitor_Enumerate(fresh);
    /* Post even on alloc failure (fresh == NULL) so the busy flag is cleared. */
    PostMessageW(hwnd, WM_APP_RESCAN_DONE, 0, (LPARAM)fresh);
    return 0;
}

/* Launch a single-flight rescan. Called only on the main thread. If one is
   already running, remember to run once more when it finishes (coalesces the
   burst of triggers that a single unlock produces). */
static void StartRescan(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_rescanBusy, 1, 0) != 0) {
        g_rescanPending = TRUE;
        return;
    }
    HANDLE h = CreateThread(NULL, 0, RescanThreadProc, hwnd, 0, NULL);
    if (h) CloseHandle(h);
    else   g_rescanBusy = 0;   /* launch failed; keep the current list */
}

/* Debounced trigger: SetTimer with the same id restarts the interval, so a
   burst of notifications collapses into one rescan once things settle. */
static void ScheduleRescan(HWND hwnd)
{
    SetTimer(hwnd, RESCAN_TIMER_ID, RESCAN_DEBOUNCE_MS, NULL);
}

/* ---- Schedule runtime ---- */

static int CurrentMinuteOfDay(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour * 60 + st.wMinute;
}

/* Push the schedule's brightness for the current time, unless suspended,
   disabled, or empty. Applies only when the value changed (less DDC traffic). */
static void Schedule_ApplyNow(void)
{
    if (!g_settings.scheduleEnabled || g_settings.scheduleCount == 0)
        return;

    int now = CurrentMinuteOfDay();

    if (g_scheduleSuspended) {
        if (Schedule_ShouldResume(g_scheduleSuspendMinute, g_scheduleResumeMinute, now))
            g_scheduleSuspended = FALSE;
        else
            return;
    }

    int value = Schedule_BrightnessAt(g_settings.schedule, g_settings.scheduleCount, now);
    if (value == g_scheduleLastApplied)
        return;

    g_scheduleLastApplied = value;
    g_masterTarget = value;
    Monitor_SetAllBrightness(&g_monitors, value);
    UI_RefreshPopup(g_hwndPopup, &g_monitors);  /* no-op if popup hidden */
}

/* A manual brightness change: hand control back to the user until the next anchor. */
static void Schedule_Suspend(void)
{
    if (!g_settings.scheduleEnabled || g_settings.scheduleCount == 0)
        return;
    int now = CurrentMinuteOfDay();
    g_scheduleSuspended = TRUE;
    g_scheduleSuspendMinute = now;
    g_scheduleResumeMinute =
        Schedule_NextAnchorMinute(g_settings.schedule, g_settings.scheduleCount, now);
    g_scheduleLastApplied = -1;  /* force re-apply after resume */
}

/* ---- Main Window Proc ---- */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* A newer instance is launching and wants our spot: exit cleanly so it can
       take over. Registered message, so it cannot be a compile-time switch case. */
    if (msg == g_wmTakeover && g_wmTakeover != 0) {
        DestroyWindow(hwnd);   /* -> WM_DESTROY -> PostQuitMessage */
        return 0;
    }

    switch (msg) {
    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            UI_TogglePopup(g_hwndPopup, &g_monitors);
            break;
        case WM_RBUTTONUP:
            ShowContextMenu(hwnd);
            break;
        }
        return 0;

    case WM_HOTKEY:
        HandleHotkey((int)wParam);
        return 0;

    case WM_COMMAND: {
        int cmd = LOWORD(wParam);
        if (cmd >= IDM_PRESET_BASE && cmd < IDM_PRESET_BASE + MAX_PRESETS) {
            ApplyPreset(cmd - IDM_PRESET_BASE);
        } else switch (cmd) {
        case IDM_RESCAN:
            StartRescan(hwnd);
            break;
        case IDM_AUTOSTART: {
            BOOL current = Settings_GetAutostart();
            Settings_SetAutostart(!current);
            g_settings.autostart = !current;
            Settings_Save(&g_settings);
            break;
        }
        case IDM_SCHEDULE_TOGGLE:
            g_settings.scheduleEnabled = !g_settings.scheduleEnabled;
            Settings_Save(&g_settings);
            g_scheduleSuspended = FALSE;      /* re-enable takes effect immediately */
            g_scheduleLastApplied = -1;
            Schedule_ApplyNow();
            break;
        case IDM_SCHEDULE_EDIT:
            UI_ShowScheduleEditor(hwnd, &g_settings);
            break;
        case IDM_SCHEDULE_SAVED:
            Settings_Save(&g_settings);
            g_scheduleSuspended = FALSE;
            g_scheduleLastApplied = -1;
            Schedule_ApplyNow();
            break;
        case IDM_ABOUT:
            UI_ShowAbout(hwnd);
            break;
        case IDM_EXIT:
            PostQuitMessage(0);
            break;
        }
        return 0;
    }

    case WM_DISPLAYCHANGE:
        /* Monitor plugged/unplugged (resolution/topology change) */
        ScheduleRescan(hwnd);
        return 0;

    case WM_WTSSESSION_CHANGE:
        /* Session unlocked or reconnected: DDC handles may be stale */
        if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT)
            ScheduleRescan(hwnd);
        return 0;

    case WM_POWERBROADCAST:
        /* Display powered back on (GUID_CONSOLE_DISPLAY_STATE) or system resume */
        if (wParam == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *pbs = (POWERBROADCAST_SETTING *)lParam;
            if (pbs &&
                IsEqualGUID(&pbs->PowerSetting, &kGuidConsoleDisplayState) &&
                pbs->DataLength >= 1 && pbs->Data[0] != 0) {  /* 0 = off, non-zero = on/dimmed */
                ScheduleRescan(hwnd);
            }
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            ScheduleRescan(hwnd);
        }
        return TRUE;

    case WM_TIMER:
        if (wParam == RESCAN_TIMER_ID) {
            KillTimer(hwnd, RESCAN_TIMER_ID);
            StartRescan(hwnd);
        } else if (wParam == SCHEDULE_TIMER_ID) {
            Schedule_ApplyNow();
        }
        return 0;

    case WM_APP_RESCAN_DONE: {
        /* Worker finished enumerating. Swap in the fresh list and rebuild the
           popup here on the UI thread (window ops must not run on the worker). */
        MonitorList *fresh = (MonitorList *)lParam;
        if (fresh) {
            Monitor_Cleanup(&g_monitors);   /* release the now-stale handles */
            g_monitors = *fresh;            /* adopt fresh list (plain struct copy) */
            free(fresh);
            Settings_LoadDeltas(&g_settings, &g_monitors);
            if (g_hwndPopup) DestroyWindow(g_hwndPopup);
            g_hwndPopup = UI_CreatePopup(g_hInst, &g_monitors);
        }
        g_rescanBusy = 0;
        if (g_rescanPending) {   /* triggers arrived mid-run: coalesce one more */
            g_rescanPending = FALSE;
            ScheduleRescan(hwnd);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
