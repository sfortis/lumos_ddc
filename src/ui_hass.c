#include "ui_internal.h"
#include "hass.h"
#include "hassurl.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>

/* ---- Home Assistant window ----
 *
 * Unlike the other windows this one is not layered: child EDIT controls are
 * not drawn inside an UpdateLayeredWindow surface, and real text fields are
 * the point of this window (the URL and the token are pasted). Standard
 * controls also give screen readers names, roles and values with no extra
 * work, the labels being STATIC controls placed before each field.
 *
 * It does not close on a click outside, because the user switches to a
 * browser to copy the token; it closes on Cancel, Save and Esc. For the same
 * reason it has a taskbar button (WS_EX_APPWINDOW). */

static const WCHAR HASS_CLASS[] = L"LumosHass";

#define HASS_WIDTH   440
#define HASS_HEIGHT  478
#define HASS_M       16
#define HASS_ITEM_H  40
#define HASS_FIELD_H 30

enum {
    IDC_URL_LABEL = 101, IDC_URL, IDC_TOKEN_LABEL, IDC_TOKEN, IDC_CONNECT,
    IDC_STATUS, IDC_LIST_LABEL, IDC_LIST
};

#define WM_APP_HASS_LIST (WM_APP + 40)

typedef struct {
    WCHAR url[HASS_URL_MAX];
    char  token[HASS_TOKEN_MAX];
    HWND  hwnd;
    DWORD gen;
} ListJob;

typedef struct {
    DWORD      gen;
    HassStatus status;
    int        count;
    HassSensor sensors[HASS_MAX_SENSORS];
} ListResult;

static struct {
    HWND       hwnd, url, token, connect, status, list;
    HFONT      font, fontTitle;
    HBRUSH     surface;
    Settings  *settings;
    HWND       notify;
    HassSensor sensors[HASS_MAX_SENSORS];
    int        count;
    BOOL       busy;
    BOOL       statusError;
    DWORD      gen;   /* a list result from an older Connect is dropped */
} g_ha;

/* ---- Layout ---- */

static int FieldTop(int i)   { return 76 + i * 62; }     /* URL, token */
static int ConnectTop(void)  { return FieldTop(2) - 8; }
static int ListTop(void)     { return ConnectTop() + 64; }
static int ListHeight(void)  { return 4 * HASS_ITEM_H; }

static void FieldRect(int i, RECT *rc)
{
    SetRect(rc, HASS_M, FieldTop(i), HASS_WIDTH - HASS_M, FieldTop(i) + HASS_FIELD_H);
}

/* ---- Worker ---- */

static DWORD WINAPI ListThread(LPVOID param)
{
    ListJob *job = (ListJob *)param;
    ListResult *res = (ListResult *)calloc(1, sizeof(ListResult));
    if (res) {
        res->gen = job->gen;
        res->status = Hass_ListSensors(job->url, job->token, res->sensors,
                                       HASS_MAX_SENSORS, &res->count);
    }
    /* Posted even without memory (res NULL), so the window does not stay busy. */
    if (!PostMessageW(job->hwnd, WM_APP_HASS_LIST, job->gen, (LPARAM)res))
        free(res);   /* the window is gone */
    SecureZeroMemory(job->token, sizeof(job->token));
    free(job);
    return 0;
}

/* ---- Helpers ---- */

static void SetStatus(const WCHAR *text, BOOL error)
{
    g_ha.statusError = error;
    SetWindowTextW(g_ha.status, text);
    InvalidateRect(g_ha.status, NULL, TRUE);
}

/* Compare two URLs the way the token cares about: the same text, ignoring
   case and trailing slashes. */
static BOOL SameUrl(const WCHAR *a, const WCHAR *b)
{
    int la = lstrlenW(a), lb = lstrlenW(b);
    while (la > 0 && a[la - 1] == L'/') la--;
    while (lb > 0 && b[lb - 1] == L'/') lb--;
    return la == lb && CompareStringOrdinal(a, la, b, lb, TRUE) == CSTR_EQUAL;
}

/* The token for the URL in the field: the one typed, or the saved one when
   the field is empty. The saved token is only ever sent to the URL it was
   saved with, so a changed URL needs the token typed again. Returns a status
   message for the user when there is no usable token, or NULL. */
static const WCHAR *CurrentToken(const WCHAR *url, char *out, int cap)
{
    out[0] = '\0';
    WCHAR typed[HASS_TOKEN_MAX];
    GetWindowTextW(g_ha.token, typed, HASS_TOKEN_MAX);
    const WCHAR *problem = NULL;
    if (typed[0]) {
        if (!WideCharToMultiByte(CP_UTF8, 0, typed, -1, out, cap, NULL, NULL)) {
            out[0] = '\0';
            problem = L"The token is too long.";
        }
    } else if (!g_ha.settings->haToken[0]) {
        problem = L"Enter an access token.";
    } else if (!SameUrl(url, g_ha.settings->haUrl)) {
        problem = L"The URL changed: enter the token again for the new address.";
    } else {
        lstrcpynA(out, g_ha.settings->haToken, cap);
    }
    SecureZeroMemory(typed, sizeof(typed));
    return problem;
}

/* An http URL sends the token unencrypted. It is allowed, because many
   Home Assistant installations on a home network have no https, but the
   window says so (user decision): in red for an address outside the home
   network, as a grey note for one on it, where most users connect. */
static const WCHAR kHttpWarning[] =
    L"This is http: the token travels unencrypted. Use https if you can.";
static const WCHAR kHttpLocalNote[] =
    L"http on the local network: the token is not encrypted on the LAN.";

/* The address in the field, normalized: trimmed, with http:// added when it
   has no scheme. Empty when the field is empty. FALSE when the address is too
   long to take http:// in front; out is empty then too. */
static BOOL ReadUrl(WCHAR *out)
{
    WCHAR typed[HASS_URL_MAX];
    GetWindowTextW(g_ha.url, typed, HASS_URL_MAX);
    if (HassUrl_Normalize(typed, out, HASS_URL_MAX))
        return TRUE;
    out[0] = L'\0';
    for (const WCHAR *p = typed; *p; p++)
        if (!iswspace(*p))
            return FALSE;
    return TRUE;
}

static const WCHAR kUrlTooLong[] = L"The address is too long.";

/* Show the http warning or note for this URL. For https, a warning or note
   left from an earlier http address is cleared. Returns FALSE for https. */
static BOOL ShowHttpStatus(const WCHAR *url)
{
    if (!HassUrl_IsHttp(url)) {
        WCHAR shown[96];
        GetWindowTextW(g_ha.status, shown, 96);
        if (lstrcmpW(shown, kHttpWarning) == 0 || lstrcmpW(shown, kHttpLocalNote) == 0)
            SetStatus(L"", FALSE);
        return FALSE;
    }
    if (HassUrl_IsLocal(url))
        SetStatus(kHttpLocalNote, FALSE);
    else
        SetStatus(kHttpWarning, TRUE);
    return TRUE;
}

/* The list belongs to the server it came from. */
static void ClearList(void)
{
    g_ha.count = 0;
    SendMessageW(g_ha.list, LB_RESETCONTENT, 0, 0);
}

static void StartList(void)
{
    if (g_ha.busy)
        return;
    ListJob *job = (ListJob *)calloc(1, sizeof(ListJob));
    if (!job)
        return;
    if (!ReadUrl(job->url)) {
        SetStatus(kUrlTooLong, TRUE);
        free(job);
        return;
    }
    /* Show the address that is used, so "192.168.1.10:8123" becomes
       "http://192.168.1.10:8123" in the field. */
    WCHAR shown[HASS_URL_MAX];
    GetWindowTextW(g_ha.url, shown, HASS_URL_MAX);
    if (job->url[0] && lstrcmpW(shown, job->url) != 0)
        SetWindowTextW(g_ha.url, job->url);
    const WCHAR *problem = job->url[0] ? CurrentToken(job->url, job->token, HASS_TOKEN_MAX)
                                       : L"Enter the URL of Home Assistant.";
    if (problem) {
        SetStatus(problem, TRUE);
        SecureZeroMemory(job->token, sizeof(job->token));
        free(job);
        return;
    }
    job->hwnd = g_ha.hwnd;
    job->gen = ++g_ha.gen;
    HANDLE h = CreateThread(NULL, 0, ListThread, job, 0, NULL);
    if (!h) {
        SecureZeroMemory(job->token, sizeof(job->token));
        free(job);
        return;
    }
    CloseHandle(h);
    g_ha.busy = TRUE;
    EnableWindow(g_ha.connect, FALSE);
    SetStatus(L"Connecting...", FALSE);
}

static int CompareSensors(const void *a, const void *b)
{
    const HassSensor *x = (const HassSensor *)a, *y = (const HassSensor *)b;
    /* Sensors without an area go last. */
    if (!x->area[0] != !y->area[0])
        return x->area[0] ? -1 : 1;
    int c = lstrcmpiW(x->area, y->area);
    return c ? c : lstrcmpiW(x->name, y->name);
}

static void FormatValue(const HassSensor *s, WCHAR *out, int cap)
{
    if (s->hasValue)
        _snwprintf(out, cap - 1, L"%.0f lx", s->lux);
    else
        lstrcpynW(out, L"no reading", cap);
    out[cap - 1] = L'\0';
}

static void FillList(void)
{
    SendMessageW(g_ha.list, LB_RESETCONTENT, 0, 0);
    int select = -1;
    for (int i = 0; i < g_ha.count; i++) {
        const HassSensor *s = &g_ha.sensors[i];
        WCHAR value[32], text[400];
        FormatValue(s, value, 32);
        /* The item text is what a screen reader reads; the drawing uses the
           sensor fields directly. */
        _snwprintf(text, 399, L"%s, %s, %s", s->name, s->area[0] ? s->area : L"no area", value);
        text[399] = L'\0';
        int idx = (int)SendMessageW(g_ha.list, LB_ADDSTRING, 0, (LPARAM)text);
        SendMessageW(g_ha.list, LB_SETITEMDATA, idx, i);
        if (lstrcmpW(s->entityId, g_ha.settings->haSensor) == 0)
            select = idx;
    }
    if (select >= 0)
        SendMessageW(g_ha.list, LB_SETCURSEL, select, 0);
}

static void OnListResult(DWORD gen, ListResult *res)
{
    if (gen != g_ha.gen) {
        free(res);
        return;
    }
    g_ha.busy = FALSE;
    EnableWindow(g_ha.connect, TRUE);
    if (!res || res->status != HASS_OK) {
        ClearList();   /* a failed Connect leaves no list from an earlier server */
        SetStatus(res ? Hass_StatusText(res->status) : L"Out of memory.", TRUE);
        free(res);
        return;
    }
    g_ha.count = res->count;
    memcpy(g_ha.sensors, res->sensors, sizeof(HassSensor) * (size_t)res->count);
    free(res);
    qsort(g_ha.sensors, (size_t)g_ha.count, sizeof(HassSensor), CompareSensors);
    FillList();
    WCHAR url[HASS_URL_MAX];
    ReadUrl(url);
    if (HassUrl_IsHttp(url) && !HassUrl_IsLocal(url)) {
        SetStatus(kHttpWarning, TRUE);   /* worth more than the count */
        return;
    }
    /* On the home network the http note joins the count in one grey line. */
    const WCHAR *how = HassUrl_IsHttp(url) ? L"Connected over http on the LAN" : L"Connected";
    WCHAR msg[128];
    if (g_ha.count == 0)
        _snwprintf(msg, 127, L"%s, but Home Assistant has no illuminance sensor.", how);
    else
        _snwprintf(msg, 127, L"%s. Choose the sensor in the room of this PC (%d found).", how, g_ha.count);
    msg[127] = L'\0';
    SetStatus(msg, FALSE);
}

static void Save(void)
{
    Settings *s = g_ha.settings;
    WCHAR url[HASS_URL_MAX];
    if (!ReadUrl(url)) {
        SetStatus(kUrlTooLong, TRUE);
        return;   /* saving it would erase the stored address */
    }
    BOOL urlChanged = !SameUrl(url, s->haUrl);
    char token[HASS_TOKEN_MAX] = "";   /* stays empty when the URL is cleared */
    const WCHAR *problem = url[0] ? CurrentToken(url, token, HASS_TOKEN_MAX) : NULL;
    if (problem) {
        SecureZeroMemory(token, sizeof(token));
        SetStatus(problem, TRUE);
        return;   /* never store a token next to a URL it was not given for */
    }
    lstrcpynW(s->haUrl, url, HASS_URL_MAX);
    SecureZeroMemory(s->haToken, sizeof(s->haToken));   /* no tail of a longer old token */
    lstrcpynA(s->haToken, token, HASS_TOKEN_MAX);
    SecureZeroMemory(token, sizeof(token));
    int sel = (int)SendMessageW(g_ha.list, LB_GETCURSEL, 0, 0);
    if (sel < 0 && urlChanged) {
        /* The chosen sensor belonged to the old server. */
        s->haSensor[0] = L'\0';
        s->haSensorLabel[0] = L'\0';
    }
    if (sel >= 0) {
        int i = (int)SendMessageW(g_ha.list, LB_GETITEMDATA, sel, 0);
        const HassSensor *hs = &g_ha.sensors[i];
        lstrcpynW(s->haSensor, hs->entityId, HASS_ENTITY_MAX);
        if (hs->area[0])
            _snwprintf(s->haSensorLabel, 199, L"%s: %s", hs->area, hs->name);
        else
            lstrcpynW(s->haSensorLabel, hs->name, 200);
        s->haSensorLabel[199] = L'\0';
    }
    PostMessageW(g_ha.notify, WM_COMMAND, IDM_HASS_SAVED, 0);
    DestroyWindow(g_ha.hwnd);
}

/* ---- Drawing ---- */

static void DrawButton(const DRAWITEMSTRUCT *di)
{
    HDC dc = di->hDC;
    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    FillRect(dc, &di->rcItem, bg);
    DeleteObject(bg);
    WCHAR label[32];
    GetWindowTextW(di->hwndItem, label, 32);
    SetBkMode(dc, TRANSPARENT);
    DrawDialogButton(dc, &di->rcItem, label, di->CtlID == IDOK, g_ha.font);
    if (di->itemState & ODS_DISABLED) {
        /* Dim a disabled button with the background color over it. */
        RECT r = di->rcItem;
        SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
        HBRUSH s = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
        HPEN oldPen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        HBRUSH oldBr = (HBRUSH)SelectObject(dc, s);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
        SelectObject(dc, oldBr);
        SelectObject(dc, oldPen);
        DeleteObject(s);
        HFONT of = (HFONT)SelectObject(dc, g_ha.font);
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
    if (di->itemState & ODS_FOCUS)
        DrawFocusRing(dc, &di->rcItem, 8);
}

static void DrawListItem(const DRAWITEMSTRUCT *di)
{
    if ((int)di->itemID < 0)
        return;
    HDC dc = di->hDC;
    BOOL selected = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(HexToColorRef(selected ? CLR_TRACK : CLR_SURFACE));
    FillRect(dc, &di->rcItem, bg);
    DeleteObject(bg);
    int i = (int)di->itemData;
    if (i < 0 || i >= g_ha.count)
        return;
    const HassSensor *s = &g_ha.sensors[i];
    SetBkMode(dc, TRANSPARENT);
    HFONT of = (HFONT)SelectObject(dc, g_ha.font);
    RECT r = di->rcItem;
    r.left += 10; r.right -= 10; r.top += 3;
    RECT line1 = r; line1.bottom = r.top + 18;
    SetTextColor(dc, HexToColorRef(selected ? CLR_ACCENT : CLR_TEXT));
    DrawTextW(dc, s->name, -1, &line1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    WCHAR value[32], second[120];
    FormatValue(s, value, 32);
    _snwprintf(second, 119, L"%s   %s", s->area[0] ? s->area : L"No area", value);
    second[119] = L'\0';
    RECT line2 = r; line2.top = r.top + 18;
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    DrawTextW(dc, second, -1, &line2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    if (di->itemState & ODS_FOCUS) {
        RECT f = di->rcItem;
        InflateRect(&f, -1, -1);
        DrawFocusRing(dc, &f, 4);
    }
}

static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    HFONT of = (HFONT)SelectObject(dc, g_ha.fontTitle);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    RECT title = { HASS_M, 16, HASS_WIDTH - HASS_M, 40 };
    DrawTextW(dc, L"Home Assistant", -1, &title, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, of);

    /* Rounded field backgrounds behind the borderless EDIT controls and the list. */
    HPEN oldPen = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, g_ha.surface);
    for (int i = 0; i < 2; i++) {
        RECT f;
        FieldRect(i, &f);
        RoundRect(dc, f.left, f.top, f.right + 1, f.bottom + 1, 8, 8);
    }
    RECT lr = { HASS_M, ListTop(), HASS_WIDTH - HASS_M, ListTop() + ListHeight() };
    InflateRect(&lr, 2, 2);
    RoundRect(dc, lr.left, lr.top, lr.right + 1, lr.bottom + 1, 8, 8);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);

    /* A ring around the focused field. */
    HWND focus = GetFocus();
    for (int i = 0; i < 2; i++) {
        if (focus == (i ? g_ha.token : g_ha.url)) {
            RECT f;
            FieldRect(i, &f);
            DrawFocusRing(dc, &f, 8);
        }
    }
    EndPaint(hwnd, &ps);
}

/* ---- Window ---- */

static HWND MakeChild(const WCHAR *cls, const WCHAR *text, DWORD style, int id,
                      int x, int y, int w, int h)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             g_ha.hwnd, (HMENU)(INT_PTR)id, g_uiInst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_ha.font, FALSE);
    return c;
}

static void CreateControls(void)
{
    int w = HASS_WIDTH - 2 * HASS_M;
    RECT f;

    MakeChild(L"STATIC", L"URL", SS_LEFT, IDC_URL_LABEL, HASS_M, FieldTop(0) - 20, w, 18);
    FieldRect(0, &f);
    g_ha.url = MakeChild(L"EDIT", g_ha.settings->haUrl, ES_AUTOHSCROLL | WS_TABSTOP, IDC_URL,
                         f.left + 8, f.top + 7, f.right - f.left - 16, 18);
    SendMessageW(g_ha.url, EM_LIMITTEXT, HASS_URL_MAX - 1, 0);   /* longer would be cut silently */

    const WCHAR *tokenLabel = g_ha.settings->haToken[0]
        ? L"Access token (one is saved; leave empty to keep it)"
        : L"Access token (a long-lived token of a non-admin user)";
    MakeChild(L"STATIC", tokenLabel, SS_LEFT, IDC_TOKEN_LABEL, HASS_M, FieldTop(1) - 20, w, 18);
    FieldRect(1, &f);
    g_ha.token = MakeChild(L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, IDC_TOKEN,
                           f.left + 8, f.top + 7, f.right - f.left - 16, 18);
    SendMessageW(g_ha.token, EM_LIMITTEXT, HASS_TOKEN_MAX - 1, 0);

    g_ha.connect = MakeChild(L"BUTTON", L"Connect", BS_OWNERDRAW | WS_TABSTOP, IDC_CONNECT,
                             HASS_M, ConnectTop(), DLG_BTN_W + 16, DLG_BTN_H);
    g_ha.status = MakeChild(L"STATIC", L"", SS_LEFT, IDC_STATUS,
                            HASS_M + DLG_BTN_W + 28, ConnectTop() - 2,
                            w - DLG_BTN_W - 28, 34);

    MakeChild(L"STATIC", L"Illuminance sensors", SS_LEFT, IDC_LIST_LABEL,
              HASS_M, ListTop() - 26, w, 18);
    g_ha.list = MakeChild(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY |
                          LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, IDC_LIST,
                          HASS_M, ListTop(), w, ListHeight());
    SetWindowTheme(g_ha.list, L"DarkMode_Explorer", NULL);   /* dark scroll bar where available */

    RECT rcCancel, rcSave;
    DialogButtonRects(HASS_WIDTH, HASS_HEIGHT - HASS_M - DLG_BTN_H, &rcCancel, &rcSave);
    MakeChild(L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP, IDCANCEL,
              rcCancel.left, rcCancel.top, DLG_BTN_W, DLG_BTN_H);
    MakeChild(L"BUTTON", L"Save", BS_OWNERDRAW | WS_TABSTOP, IDOK,
              rcSave.left, rcSave.top, DLG_BTN_W, DLG_BTN_H);
}

static LRESULT CALLBACK HassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wParam, HexToColorRef(CLR_TEXT));
        SetBkColor((HDC)wParam, HexToColorRef(CLR_SURFACE));
        return (LRESULT)g_ha.surface;

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wParam;
        BOOL isStatus = ((HWND)lParam == g_ha.status);
        SetTextColor(dc, HexToColorRef(isStatus && g_ha.statusError ? CLR_ERROR : CLR_SUBTEXT));
        SetBkColor(dc, HexToColorRef(CLR_BG));
        static HBRUSH bg;
        if (!bg) bg = CreateSolidBrush(HexToColorRef(CLR_BG));
        return (LRESULT)bg;
    }

    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lParam;
        if (mi->CtlID == IDC_LIST)
            mi->itemHeight = HASS_ITEM_H;
        return TRUE;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lParam;
        if (di->CtlType == ODT_LISTBOX)
            DrawListItem(di);
        else
            DrawButton(di);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CONNECT:
            StartList();
            return 0;
        case IDOK:
            Save();
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_URL:
        case IDC_TOKEN:
            /* Repaint the focus ring as the focus moves between fields. */
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)
                InvalidateRect(hwnd, NULL, FALSE);
            if (LOWORD(wParam) == IDC_URL && HIWORD(wParam) == EN_CHANGE) {
                WCHAR url[HASS_URL_MAX];
                ReadUrl(url);
                /* A list from another address must not be saved with this one,
                   neither one shown already nor one still on its way. */
                if (g_ha.busy) {
                    g_ha.gen++;
                    g_ha.busy = FALSE;
                    EnableWindow(g_ha.connect, TRUE);
                    SetStatus(L"Connect to list the sensors of this address.", FALSE);
                }
                if (g_ha.count > 0) {
                    ClearList();
                    SetStatus(L"Connect to list the sensors of this address.", FALSE);
                }
                ShowHttpStatus(url);
            }
            return 0;
        }
        break;

    case WM_APP_HASS_LIST:
        OnListResult((DWORD)wParam, (ListResult *)lParam);
        return 0;

    case WM_LBUTTONDOWN:
        BeginWindowDrag(hwnd);   /* anywhere that is not a control moves the window */
        return 0;

    case WM_DESTROY: {
        /* A result posted before the window went away would be dropped with
           its memory; take it out of the queue and free it. */
        MSG pending;
        while (PeekMessageW(&pending, hwnd, WM_APP_HASS_LIST, WM_APP_HASS_LIST, PM_REMOVE))
            free((ListResult *)pending.lParam);
        g_ha.gen++;   /* a list still on its way is dropped by its worker */
        g_ha.busy = FALSE;
        if (g_ha.font) DeleteObject(g_ha.font);
        if (g_ha.fontTitle) DeleteObject(g_ha.fontTitle);
        if (g_ha.surface) DeleteObject(g_ha.surface);
        g_ha.font = g_ha.fontTitle = NULL;
        g_ha.surface = NULL;
        g_ha.hwnd = NULL;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UiHass_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = HassWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_LUMOS));
    wc.lpszClassName = HASS_CLASS;
    RegisterClassExW(&wc);
}

void UI_ShowHomeAssistant(HWND hwndOwner, Settings *s, HWND notify)
{
    if (g_ha.hwnd && IsWindow(g_ha.hwnd)) {
        SetForegroundWindow(g_ha.hwnd);
        return;
    }
    g_ha.settings = s;
    g_ha.notify = notify;
    g_ha.count = 0;
    g_ha.busy = FALSE;
    g_ha.statusError = FALSE;
    g_ha.font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_ha.fontTitle = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_ha.surface = CreateSolidBrush(HexToColorRef(CLR_SURFACE));

    /* Centered on the monitor the cursor is on, like the other dialogs. */
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = (mi.rcWork.left + mi.rcWork.right - HASS_WIDTH) / 2;
    int y = (mi.rcWork.top + mi.rcWork.bottom - HASS_HEIGHT) / 2;

    g_ha.hwnd = CreateWindowExW(WS_EX_APPWINDOW, HASS_CLASS, L"Home Assistant",
                                WS_POPUP | WS_CLIPCHILDREN, x, y, HASS_WIDTH, HASS_HEIGHT,
                                hwndOwner, NULL, g_uiInst, NULL);
    if (!g_ha.hwnd) {
        DeleteObject(g_ha.font);
        DeleteObject(g_ha.fontTitle);
        DeleteObject(g_ha.surface);
        g_ha.font = g_ha.fontTitle = NULL;
        g_ha.surface = NULL;
        return;
    }
    DWORD round = 2;   /* DWMWCP_ROUND; ignored before Windows 11 */
    DwmSetWindowAttribute(g_ha.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    CreateControls();
    ShowWindow(g_ha.hwnd, SW_SHOW);
    SetForegroundWindow(g_ha.hwnd);
    SetFocus(g_ha.settings->haUrl[0] ? g_ha.list : g_ha.url);

    /* With a saved connection the list is loaded right away. */
    if (s->haUrl[0] && s->haToken[0])
        StartList();
    else
        SetStatus(L"Enter the URL and a token, then Connect.", FALSE);
}

BOOL UI_HassDialogMessage(MSG *msg)
{
    return g_ha.hwnd && IsDialogMessageW(g_ha.hwnd, msg);
}

HWND UI_HassWindow(void)
{
    return g_ha.hwnd;
}
