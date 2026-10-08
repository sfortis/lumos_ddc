#include "ui_internal.h"
#include <windowsx.h>

static const WCHAR SET_CLASS[]     = L"LumosSettings";
static HWND g_setHwnd = NULL;

/* ---- Settings window ---- */

/* Rows are data, not code: the table below drives rendering, hit testing,
   keyboard focus and the screen reader model, so adding a setting later is one
   BuildSettingsRows line. */
enum { SET_SECTION = 0, SET_TOGGLE, SET_NUMBER, SET_HOTKEY, SET_ACTION };

/* What a SET_ACTION row does when it is clicked or activated. */
enum { SET_ACT_HASS = 1, SET_ACT_RESET_CURVE };
enum { SET_UNIT_PLAIN = 0, SET_UNIT_PERCENT, SET_UNIT_MINUTES };

/* Hit kinds returned by SetHitTest. */
enum { SETHIT_NONE = 0, SETHIT_MINUS, SETHIT_PLUS, SETHIT_TOGGLE, SETHIT_HOTKEY, SETHIT_ROW, SETHIT_ACTION };

typedef struct {
    int    kind;
    WCHAR  label[MAX_PRESET_NAME + 16];
    int   *ival;      /* SET_NUMBER: the value being edited */
    BOOL  *bval;      /* SET_TOGGLE: the flag being edited */
    Hotkey *hval;     /* SET_HOTKEY: the combination being edited */
    int    lo, hi;    /* SET_NUMBER bounds */
    int    step;      /* SET_NUMBER increment (minutes scale instead, see SetStepFor) */
    int    unit;
    int    action;    /* SET_ACTION: a SET_ACT_* value */
    BOOL   divider;   /* SET_SECTION: a line above it, for every section but the first */
    int    when;      /* SET_WHEN_*: when the row can be changed */
} SetRow;

/* A row that does nothing in some state is drawn grey and cannot be changed,
   but keeps its place in the focus order so a screen reader still finds it
   (as unavailable), the way a disabled Windows control is announced. */
enum { SET_WHEN_ALWAYS = 0, SET_WHEN_NO_AUTO };

/* Space above a section that has a divider; the line sits in the middle of it. */
#define SET_DIVIDER_H 12

#define MAX_SET_ROWS (MAX_PRESETS + MAX_MONITORS + 16)

typedef struct {
    /* Working copy. Edits are discarded unless the user hits Save, which is why
       the window never writes into Settings directly. */
    int   step;
    BOOL  autostart;
    BOOL  scheduleEnabled;
    BOOL  idleDimEnabled;
    int   idleDimPercent;
    int   idleDimMinutes;
    int   presetValues[MAX_PRESETS];
    int   presetCount;
    BOOL  haAutoEnabled;
    BOOL  haResetCurve;            /* forget the learned points on Save */
    Hotkey hotkeys[HOTKEY_COUNT];

    SetRow rows[MAX_SET_ROWS];
    int    rowCount;
    int    hoverRow;
    int    focusRow;       /* keyboard focus: a row index, SET_CANCEL or SET_SAVE */
    BOOL   focusVisible;   /* draw the ring: set once the keyboard is in use */
    int    captureRow;     /* hotkey row waiting for a key combination, or -1 */
    int    errorRow;       /* hotkey row showing errorText, or -1 */
    WPARAM captureEndVk;   /* key that started or ended a capture; its repeats are dropped */
    WCHAR  errorText[64];
    Settings *settings;
    HWND   owner;

    /* The rows scroll between the fixed header and footer when they do not fit
       the work area: a laptop at 1366x768, or 1920x1080 at 150%, has about
       700 pixels, and two monitors already make the rows taller than that. */
    int    viewH;          /* height of the rows area on screen */
    int    scroll;         /* pixels of the rows scrolled out at the top */
    BOOL   thumbDrag;      /* the scroll bar thumb is being dragged */
    int    thumbDragY, thumbDragScroll;
    DWORD  lastWheelScroll;  /* GetTickCount of the last wheel notch that scrolled */
} SetEditData;

#define SET_SCROLLBAR_W 10   /* hit width at the right edge; the bar drawn is 4 */

/* A wheel notch this soon after one that scrolled keeps scrolling, even when
   the rows moving under the cursor bring a number control beneath it. */
#define SET_WHEEL_LATCH_MS 600

static SetEditData g_set;

/* The footer buttons follow the rows in focus order. */
#define SET_CANCEL(d) ((d)->rowCount)
#define SET_SAVE(d)   ((d)->rowCount + 1)
static const HotkeyHost *g_hotkeyHost = NULL;

void UI_SetHotkeyHost(const HotkeyHost *host)
{
    g_hotkeyHost = host;
}

static SetRow *SetAddRow(SetEditData *d, int kind, const WCHAR *label)
{
    if (d->rowCount >= MAX_SET_ROWS) return NULL;
    SetRow *r = &d->rows[d->rowCount++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->divider = (kind == SET_SECTION && d->rowCount > 1);
    wcsncpy(r->label, label, (sizeof(r->label) / sizeof(WCHAR)) - 1);
    return r;
}

static void SetAddToggle(SetEditData *d, const WCHAR *label, BOOL *val)
{
    SetRow *r = SetAddRow(d, SET_TOGGLE, label);
    if (r) r->bval = val;
}

static void SetAddNumber(SetEditData *d, const WCHAR *label, int *val,
                         int lo, int hi, int step, int unit)
{
    SetRow *r = SetAddRow(d, SET_NUMBER, label);
    if (!r) return;
    r->ival = val;
    r->lo = lo;
    r->hi = hi;
    r->step = step;
    r->unit = unit;
}

static void SetAddHotkey(SetEditData *d, const WCHAR *label, Hotkey *val)
{
    SetRow *r = SetAddRow(d, SET_HOTKEY, label);
    if (r) r->hval = val;
}

static void SetAddAction(SetEditData *d, const WCHAR *label, int action)
{
    SetRow *r = SetAddRow(d, SET_ACTION, label);
    if (r) r->action = action;
}

static void BuildSettingsRows(SetEditData *d)
{
    d->rowCount = 0;

    SetAddRow(d, SET_SECTION, L"GENERAL");
    SetAddNumber(d, L"Brightness step", &d->step, 1, 50, 1, SET_UNIT_PERCENT);
    SetAddToggle(d, L"Start with Windows", &d->autostart);

    SetAddRow(d, SET_SECTION, L"HOTKEYS");
    SetAddHotkey(d, L"Brighten", &d->hotkeys[HOTKEY_BRIGHTEN]);
    SetAddHotkey(d, L"Dim", &d->hotkeys[HOTKEY_DIM]);
    SetAddHotkey(d, L"Open popup", &d->hotkeys[HOTKEY_POPUP]);

    SetAddRow(d, SET_SECTION, L"IDLE DIM");
    SetAddToggle(d, L"Dim when idle", &d->idleDimEnabled);
    SetAddNumber(d, L"Idle level", &d->idleDimPercent, 0, 100, 1, SET_UNIT_PERCENT);
    SetAddNumber(d, L"Dim after", &d->idleDimMinutes, 1, 1440, 5, SET_UNIT_MINUTES);

    SetAddRow(d, SET_SECTION, L"SCHEDULE");
    SetAddToggle(d, L"Brightness schedule", &d->scheduleEnabled);
    if (d->rowCount > 0)
        d->rows[d->rowCount - 1].when = SET_WHEN_NO_AUTO;   /* auto brightness replaces it */

    SetAddRow(d, SET_SECTION, L"HOME ASSISTANT");
    SetAddAction(d, L"Configure", SET_ACT_HASS);
    SetAddToggle(d, L"Auto brightness", &d->haAutoEnabled);
    SetAddAction(d, L"Learned curve", SET_ACT_RESET_CURVE);

    if (d->presetCount > 0) {
        SetAddRow(d, SET_SECTION, L"PRESETS");
        for (int i = 0; i < d->presetCount; i++)
            SetAddNumber(d, d->settings->presets[i].name, &d->presetValues[i],
                         0, 100, 5, SET_UNIT_PERCENT);
    }
}

/* Auto brightness is on in this window's working copy and has a sensor, so
   the schedule only stands in while Home Assistant is offline. */
static BOOL SetAutoOn(const SetEditData *d)
{
    return d->haAutoEnabled && d->settings->haSensor[0];
}

static BOOL SetRowEnabled(const SetEditData *d, const SetRow *r)
{
    return r->when != SET_WHEN_NO_AUTO || !SetAutoOn(d);
}

static const WCHAR kFallbackNote[] = L"Used if Home Assistant is offline";

static int SetRowHeight(SetRow *r)
{
    if (r->kind == SET_SECTION)
        return SET_SECTION_H + (r->divider ? SET_DIVIDER_H : 0);
    return SET_ROW_H;
}

static int SetContentHeight(SetEditData *d)
{
    int h = 0;
    for (int i = 0; i < d->rowCount; i++)
        h += SetRowHeight(&d->rows[i]);
    return h;
}

/* The window: header, the visible part of the rows, footer. */
static int SetHeight(SetEditData *d)
{
    return SET_HEADER_H + d->viewH + SET_FOOTER_H;
}

static int SetMaxScroll(SetEditData *d)
{
    int m = SetContentHeight(d) - d->viewH;
    return m > 0 ? m : 0;
}

static void SetScrollTo(SetEditData *d, int scroll)
{
    int m = SetMaxScroll(d);
    d->scroll = scroll < 0 ? 0 : scroll > m ? m : scroll;
}

/* Top of a row on screen, after scrolling; outside the rows area when the
   row is scrolled out. */
static int SetRowTop(SetEditData *d, int row)
{
    int top = SET_HEADER_H - d->scroll;
    for (int i = 0; i < row; i++)
        top += SetRowHeight(&d->rows[i]);
    return top;
}

static BOOL SetRowVisible(SetEditData *d, int row)
{
    int top = SetRowTop(d, row);
    return top >= SET_HEADER_H && top + SetRowHeight(&d->rows[row]) <= SET_HEADER_H + d->viewH;
}

/* Scroll just enough to show a row, with the section title above it when it
   is the first row of its section. Cancel and Save are always shown. */
static void SetEnsureVisible(SetEditData *d, int row)
{
    if (row < 0 || row >= d->rowCount)
        return;
    int top = SetRowTop(d, row) + d->scroll - SET_HEADER_H;   /* in content coordinates */
    if (row > 0 && d->rows[row - 1].kind == SET_SECTION)
        top -= SetRowHeight(&d->rows[row - 1]);
    int bottom = SetRowTop(d, row) + d->scroll - SET_HEADER_H + SetRowHeight(&d->rows[row]);
    if (top < d->scroll)
        SetScrollTo(d, top);
    else if (bottom > d->scroll + d->viewH)
        SetScrollTo(d, bottom - d->viewH);
}

/* The scroll bar track and thumb, in window coordinates; FALSE when the rows
   fit and there is no bar. */
static BOOL SetScrollbarRects(SetEditData *d, RECT *track, RECT *thumb)
{
    int content = SetContentHeight(d), m = SetMaxScroll(d);
    if (m <= 0)
        return FALSE;
    SetRect(track, SET_WIDTH - 7, SET_HEADER_H + 2, SET_WIDTH - 3, SET_HEADER_H + d->viewH - 2);
    int trackH = track->bottom - track->top;
    int thumbH = trackH * d->viewH / content;
    if (thumbH < 24) thumbH = 24;
    int y = track->top + (trackH - thumbH) * d->scroll / m;
    SetRect(thumb, track->left, y, track->right, y + thumbH);
    return TRUE;
}

/* Number controls sit on the right edge: [-] value [+] */
static void SetControlRects(int top, RECT *rcMinus, RECT *rcValue, RECT *rcPlus)
{
    int t = top + 4, b = top + SET_ROW_H - 4;
    rcMinus->left = SET_WIDTH - 116; rcMinus->right = SET_WIDTH - 92;
    rcValue->left = SET_WIDTH - 90;  rcValue->right = SET_WIDTH - 44;
    rcPlus->left  = SET_WIDTH - 40;  rcPlus->right  = SET_WIDTH - 16;
    rcMinus->top = rcValue->top = rcPlus->top = t;
    rcMinus->bottom = rcValue->bottom = rcPlus->bottom = b;
}

/* The wheel changes a number only over its [-] value [+] control, so a wheel
   meant to scroll is not caught by the label of a row passing under it. */
static BOOL SetOverNumberControl(SetEditData *d, int row, int x, int y)
{
    RECT rcMinus, rcValue, rcPlus;
    SetControlRects(SetRowTop(d, row), &rcMinus, &rcValue, &rcPlus);
    return x >= rcMinus.left && x <= rcPlus.right && y >= rcMinus.top && y < rcMinus.bottom;
}

static void SetToggleRect(int top, RECT *rc)
{
    rc->right  = SET_WIDTH - 16;
    rc->left   = rc->right - 36;
    rc->top    = top + (SET_ROW_H - 20) / 2;
    rc->bottom = rc->top + 20;
}

/* Hotkey text box: wider than the number controls, "Ctrl+Alt+PageDown" fits. */
static void SetHotkeyRect(int top, RECT *rc)
{
    rc->right  = SET_WIDTH - 16;
    rc->left   = SET_WIDTH - 176;
    rc->top    = top + 4;
    rc->bottom = top + SET_ROW_H - 4;
}

static void SetButtonRects(SetEditData *d, RECT *rcCancel, RECT *rcSave)
{
    DialogButtonRects(SET_WIDTH, SetHeight(d) - SET_FOOTER_H + 10, rcCancel, rcSave);
}

/* Minutes run from 1 to 1440, so the increment scales with the value: a fixed
   step is either too coarse near 1 or takes hundreds of clicks near 1440. */
static int SetStepFor(SetRow *r, int value)
{
    if (r->unit != SET_UNIT_MINUTES) return r->step;
    if (value < 15) return 1;
    if (value < 60) return 5;
    return 15;
}

static void SetAdjust(SetRow *r, int dir)
{
    if (r->kind != SET_NUMBER || !r->ival) return;
    int v = *r->ival;
    /* Stepping down uses the bucket below the current value, so the same click
       count walks a value back to where it came from. */
    v += (dir > 0) ? SetStepFor(r, v) : -SetStepFor(r, v - 1);
    if (v < r->lo) v = r->lo;
    if (v > r->hi) v = r->hi;
    *r->ival = v;
}

/* The text on the right of an action row, also read by screen readers. */
static void SetActionText(SetEditData *d, SetRow *r, WCHAR *buf, int cch)
{
    const Settings *s = d->settings;
    if (r->action == SET_ACT_HASS) {
        lstrcpynW(buf, s->haSensor[0] ? (s->haSensorLabel[0] ? s->haSensorLabel : s->haSensor)
                                      : L"Not set up", cch);
    } else if (d->haResetCurve) {
        lstrcpynW(buf, L"Reset on Save", cch);
    } else if (s->haCurve.count == 0) {
        lstrcpynW(buf, L"Default", cch);
    } else {
        _snwprintf(buf, cch - 1, L"%d points, click to reset", s->haCurve.count);
        buf[cch - 1] = L'\0';
    }
}

/* Returns the row under (x,y) and sets *outHit, or -1 for none. */
static int SetHitTest(SetEditData *d, int x, int y, int *outHit)
{
    *outHit = SETHIT_NONE;
    if (y < SET_HEADER_H || y >= SET_HEADER_H + d->viewH)
        return -1;   /* the header or the footer: rows scrolled under them do not count */
    int top = SET_HEADER_H - d->scroll;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];
        int rh = SetRowHeight(r);
        if (y >= top && y < top + rh) {
            if (r->kind == SET_SECTION) return -1;
            if (!SetRowEnabled(d, r)) {
                *outHit = SETHIT_ROW;   /* like a label: it moves the window */
                return i;
            }
            if (r->kind == SET_ACTION) {
                *outHit = SETHIT_ACTION;
                return i;
            }
            if (r->kind == SET_TOGGLE) {
                RECT rc;
                SetToggleRect(top, &rc);
                *outHit = (x >= rc.left && x <= rc.right) ? SETHIT_TOGGLE : SETHIT_ROW;
                return i;
            }
            if (r->kind == SET_HOTKEY) {
                RECT rc;
                SetHotkeyRect(top, &rc);
                *outHit = (x >= rc.left && x <= rc.right) ? SETHIT_HOTKEY : SETHIT_ROW;
                return i;
            }
            RECT rcMinus, rcValue, rcPlus;
            SetControlRects(top, &rcMinus, &rcValue, &rcPlus);
            if (x >= rcMinus.left && x <= rcMinus.right)     *outHit = SETHIT_MINUS;
            else if (x >= rcPlus.left && x <= rcPlus.right)   *outHit = SETHIT_PLUS;
            else                                             *outHit = SETHIT_ROW;
            return i;
        }
        top += rh;
    }
    return -1;
}

/* ---- Value text, shared by the renderer and the screen reader ---- */

static void SetNumberText(SetRow *r, BOOL spoken, WCHAR *buf, int cch)
{
    int v = r->ival ? *r->ival : 0;
    if (r->unit == SET_UNIT_MINUTES)
        _snwprintf(buf, cch - 1, spoken ? (v == 1 ? L"%d minute" : L"%d minutes") : L"%dm", v);
    else if (r->unit == SET_UNIT_PERCENT)
        _snwprintf(buf, cch - 1, L"%d%%", v);
    else
        _snwprintf(buf, cch - 1, L"%d", v);
    buf[cch - 1] = L'\0';
}

static void SetHotkeyText(SetEditData *d, int row, BOOL spoken, WCHAR *buf, int cch)
{
    SetRow *r = &d->rows[row];
    if (row == d->captureRow) {
        wcsncpy(buf, spoken ? L"press the new keys, Escape cancels, Backspace clears"
                            : L"Press keys...", cch - 1);
        buf[cch - 1] = L'\0';
        return;
    }
    char text[HOTKEY_TEXT_MAX];
    Hotkey_Format(*r->hval, text, HOTKEY_TEXT_MAX);
    int k = 0;
    for (; text[k] && k < cch - 1; k++)
        buf[k] = (WCHAR)(unsigned char)text[k];
    buf[k] = L'\0';
}

/* ---- Rendering ---- */

static void RenderSettings(HWND hwnd, SetEditData *d)
{
    int w = SET_WIDTH;
    int h = SetHeight(d);

    BYTE *bits = NULL;
    HBITMAP bmp = NULL;
    HDC dc = CreateAlphaDC(w, h, &bmp, &bits);

    HBRUSH bg = CreateSolidBrush(HexToColorRef(CLR_BG));
    RECT rcAll = { 0, 0, w, h };
    FillRect(dc, &rcAll, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    /* Title + hint */
    RECT rcTitle = { 16, 12, w - 16, 32 };
    SelectObject(dc, hFontBold);
    SetTextColor(dc, HexToColorRef(CLR_TEXT));
    DrawTextW(dc, L"Settings", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, hFontSmall);
    SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
    DrawTextW(dc, SetMaxScroll(d) > 0 ? L"wheel = adjust or scroll" : L"wheel = adjust", -1,
              &rcTitle, DT_RIGHT | DT_SINGLELINE);

    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, noPen);

    /* Rows draw inside the rows area only; one scrolled halfway is cut at the edge. */
    int viewTop = SET_HEADER_H, viewBottom = SET_HEADER_H + d->viewH;
    IntersectClipRect(dc, 0, viewTop, w, viewBottom);
    int y = SET_HEADER_H - d->scroll;
    for (int i = 0; i < d->rowCount; i++) {
        SetRow *r = &d->rows[i];
        if (y + SetRowHeight(r) <= viewTop || y >= viewBottom) {
            y += SetRowHeight(r);
            continue;
        }

        if (r->kind == SET_SECTION) {
            if (r->divider) {
                /* The same line as the separators of the context menu. */
                HBRUSH sep = CreateSolidBrush(HexToColorRef(CLR_TRACK));
                RECT rcSep = { 16, y + SET_DIVIDER_H / 2, w - 16, y + SET_DIVIDER_H / 2 + 1 };
                FillRect(dc, &rcSep, sep);
                DeleteObject(sep);
                y += SET_DIVIDER_H;
            }
            RECT rc = { 16, y, w - 16, y + SET_SECTION_H };
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, r->label, -1, &rc, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            y += SET_SECTION_H;
            continue;
        }

        if (i == d->hoverRow) {
            HBRUSH hb = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
            HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
            RoundRect(dc, 8, y + 2, w - 8, y + SET_ROW_H - 2, 8, 8);
            SelectObject(dc, ob);
            DeleteObject(hb);
        }

        BOOL enabled = SetRowEnabled(d, r);
        int labelRight = (r->kind == SET_HOTKEY) ? w - 184 : (r->kind == SET_ACTION) ? w - 196 : w - 124;
        RECT rcLabel = { 16, y, labelRight, y + SET_ROW_H };
        SelectObject(dc, hFont);
        SetTextColor(dc, HexToColorRef(enabled ? CLR_TEXT : CLR_SUBTEXT));
        if (!enabled) {
            /* The label moves up and the reason goes under it, in the row's own height. */
            rcLabel.bottom = y + SET_ROW_H / 2 + 3;
            DrawTextW(dc, r->label, -1, &rcLabel,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            RECT rcNote = { 16, y + SET_ROW_H / 2 + 2, labelRight, y + SET_ROW_H };
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, kFallbackNote, -1, &rcNote,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            DrawTextW(dc, r->label, -1, &rcLabel,   /* monitor and preset names may hold "&" */
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        if (r->kind == SET_ACTION) {
            WCHAR val[200];
            SetActionText(d, r, val, 200);
            RECT rcV = { w - 192, y, w - 30, y + SET_ROW_H };
            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, val, -1, &rcV,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            RECT rcC = { w - 28, y, w - 14, y + SET_ROW_H };
            SelectObject(dc, hFont);
            SetTextColor(dc, HexToColorRef(CLR_ACCENT));
            DrawTextW(dc, L"\x203A", -1, &rcC, DT_CENTER | DT_VCENTER | DT_SINGLELINE);   /* single angle quote */
        } else if (r->kind == SET_TOGGLE) {
            RECT rcT;
            SetToggleRect(y, &rcT);
            BOOL on = (r->bval && *r->bval);
            /* Disabled: the position still shows the setting, in grey. */
            HBRUSH track = CreateSolidBrush(HexToColorRef(on && enabled ? CLR_ACCENT : CLR_TRACK));
            HBRUSH knob  = CreateSolidBrush(HexToColorRef(!enabled ? CLR_SURFACE : on ? CLR_BG : CLR_SUBTEXT));
            HBRUSH ob = (HBRUSH)SelectObject(dc, track);
            RoundRect(dc, rcT.left, rcT.top, rcT.right, rcT.bottom, 20, 20);
            SelectObject(dc, knob);
            int kd = (rcT.bottom - rcT.top) - 6;   /* knob diameter, 3px inset */
            int kx = on ? rcT.right - 3 - kd : rcT.left + 3;
            Ellipse(dc, kx, rcT.top + 3, kx + kd, rcT.top + 3 + kd);
            SelectObject(dc, ob);
            DeleteObject(track);
            DeleteObject(knob);
        } else if (r->kind == SET_HOTKEY) {
            RECT rcH;
            SetHotkeyRect(y, &rcH);
            HBRUSH box = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            HBRUSH ob = (HBRUSH)SelectObject(dc, box);
            RoundRect(dc, rcH.left, rcH.top, rcH.right, rcH.bottom, 6, 6);
            SelectObject(dc, ob);
            DeleteObject(box);

            WCHAR text[64];
            COLORREF color = HexToColorRef(CLR_TEXT);
            if (i == d->errorRow) {
                wcsncpy(text, d->errorText, 63);
                text[63] = L'\0';
                color = HexToColorRef(CLR_ERROR);
            } else {
                SetHotkeyText(d, i, FALSE, text, 64);
                if (i == d->captureRow) color = HexToColorRef(CLR_ACCENT);
            }
            SelectObject(dc, (i == d->errorRow) ? hFontSmall : hFont);
            SetTextColor(dc, color);
            DrawTextW(dc, text, -1, &rcH, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            RECT rcMinus, rcValue, rcPlus;
            SetControlRects(y, &rcMinus, &rcValue, &rcPlus);

            HBRUSH btn = CreateSolidBrush(HexToColorRef(CLR_TRACK));
            HBRUSH ob = (HBRUSH)SelectObject(dc, btn);
            RoundRect(dc, rcMinus.left, rcMinus.top, rcMinus.right, rcMinus.bottom, 6, 6);
            RoundRect(dc, rcPlus.left, rcPlus.top, rcPlus.right, rcPlus.bottom, 6, 6);
            SelectObject(dc, ob);
            DeleteObject(btn);

            SelectObject(dc, hFontSmall);
            SetTextColor(dc, HexToColorRef(CLR_SUBTEXT));
            DrawTextW(dc, L"\x2013", -1, &rcMinus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, L"+", -1, &rcPlus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            WCHAR val[32];
            SetNumberText(r, FALSE, val, 32);
            SelectObject(dc, hFont);
            SetTextColor(dc, HexToColorRef(CLR_ACCENT));
            DrawTextW(dc, val, -1, &rcValue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        if (d->focusVisible && i == d->focusRow) {
            RECT rcFocus = { 6, y + 1, w - 6, y + SET_ROW_H - 1 };
            DrawFocusRing(dc, &rcFocus, 10);
        }

        y += SET_ROW_H;
    }
    SelectClipRgn(dc, NULL);

    RECT rcTrack, rcThumb;
    if (SetScrollbarRects(d, &rcTrack, &rcThumb)) {
        HBRUSH tb = CreateSolidBrush(HexToColorRef(CLR_SURFACE));
        HBRUSH th = CreateSolidBrush(HexToColorRef(d->thumbDrag ? CLR_SUBTEXT : CLR_TRACK));
        HBRUSH ob = (HBRUSH)SelectObject(dc, tb);
        RoundRect(dc, rcTrack.left, rcTrack.top, rcTrack.right + 1, rcTrack.bottom + 1, 4, 4);
        SelectObject(dc, th);
        RoundRect(dc, rcThumb.left, rcThumb.top, rcThumb.right + 1, rcThumb.bottom + 1, 4, 4);
        SelectObject(dc, ob);
        DeleteObject(tb);
        DeleteObject(th);
    }

    /* Footer: the shared Cancel and Save buttons */
    RECT rcCancel, rcSave;
    SetButtonRects(d, &rcCancel, &rcSave);
    DrawDialogButton(dc, &rcCancel, L"Cancel", FALSE, hFont);
    DrawDialogButton(dc, &rcSave, L"Save", TRUE, hFont);

    if (d->focusVisible && (d->focusRow == SET_CANCEL(d) || d->focusRow == SET_SAVE(d))) {
        RECT rcFocus = (d->focusRow == SET_SAVE(d)) ? rcSave : rcCancel;
        InflateRect(&rcFocus, 3, 3);
        DrawFocusRing(dc, &rcFocus, 12);
    }

    SelectObject(dc, oldPen);
    DeleteObject(noPen);
    DeleteObject(hFont);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);

    ApplyRoundedMask(bits, w, h, SET_CORNER, 245);
    CommitLayered(hwnd, dc, w, h);

    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- Screen reader model ----
   Section headers are drawing only. The model lists the editable rows in
   order, then the Save button, so its index and the row index differ. */

static BOOL SetRowFocusable(SetEditData *d, int row)
{
    return row == SET_CANCEL(d) || row == SET_SAVE(d) ||
           (row >= 0 && row < d->rowCount && d->rows[row].kind != SET_SECTION);
}

static int SetRowFromModel(SetEditData *d, int index)
{
    for (int i = 0; i <= SET_SAVE(d); i++)
        if (SetRowFocusable(d, i) && index-- == 0)
            return i;
    return -1;
}

static int SetModelFromRow(SetEditData *d, int row)
{
    if (!SetRowFocusable(d, row))
        return -1;
    int n = 0;
    for (int i = 0; i < row; i++)
        if (SetRowFocusable(d, i)) n++;
    return n;
}

static int SetA11yCount(void *ctx)
{
    SetEditData *d = (SetEditData *)ctx;
    return SetModelFromRow(d, SET_SAVE(d)) + 1;
}

static void SetA11yDescribe(void *ctx, int index, A11yItem *out)
{
    SetEditData *d = (SetEditData *)ctx;
    if (index < 0) {
        out->role = ROLE_SYSTEM_DIALOG;
        out->state = STATE_SYSTEM_FOCUSABLE;
        wcscpy(out->name, APP_NAME L" settings");
        return;
    }
    int row = SetRowFromModel(d, index);
    if (row < 0)
        return;
    out->state = STATE_SYSTEM_FOCUSABLE;

    if (row == SET_CANCEL(d) || row == SET_SAVE(d)) {
        RECT rcCancel, rcSave;
        SetButtonRects(d, &rcCancel, &rcSave);
        out->role = ROLE_SYSTEM_PUSHBUTTON;
        out->rect = (row == SET_SAVE(d)) ? rcSave : rcCancel;
        wcscpy(out->name, (row == SET_SAVE(d)) ? L"Save" : L"Cancel");
        wcscpy(out->action, L"Press");
        return;
    }

    SetRow *r = &d->rows[row];
    int top = SetRowTop(d, row);
    SetRect(&out->rect, 8, top, SET_WIDTH - 8, top + SET_ROW_H);
    if (!SetRowVisible(d, row))
        out->state |= STATE_SYSTEM_OFFSCREEN;

    switch (r->kind) {
    case SET_TOGGLE:
        out->role = ROLE_SYSTEM_CHECKBUTTON;
        if (r->bval && *r->bval)
            out->state |= STATE_SYSTEM_CHECKED;
        if (SetRowEnabled(d, r)) {
            wcsncpy(out->name, r->label, 159);
            wcscpy(out->action, L"Toggle");
        } else {
            out->state |= STATE_SYSTEM_UNAVAILABLE;
            _snwprintf(out->name, 159, L"%s, %s", r->label, kFallbackNote);
            out->name[159] = L'\0';
        }
        break;
    case SET_ACTION: {
        WCHAR val[200];
        SetActionText(d, r, val, 200);
        out->role = ROLE_SYSTEM_PUSHBUTTON;
        /* Section titles are not in the MSAA tree, so "Configure" alone would
           not tell a screen reader user what it configures. */
        const WCHAR *label = r->action == SET_ACT_HASS ? L"Home Assistant settings" : r->label;
        _snwprintf(out->name, 159, L"%s: %s", label, val);
        out->name[159] = L'\0';
        wcscpy(out->action, L"Press");
        break;
    }
    case SET_NUMBER:
        out->role = ROLE_SYSTEM_SPINBUTTON;
        wcsncpy(out->name, r->label, 159);
        SetNumberText(r, TRUE, out->value, 64);
        break;
    case SET_HOTKEY: {
        /* Buttons have no spoken value in every screen reader, so the
           combination (or the error) goes into the name. */
        WCHAR text[64];
        if (row == d->errorRow) {
            char hk[HOTKEY_TEXT_MAX];
            Hotkey_Format(*r->hval, hk, HOTKEY_TEXT_MAX);
            _snwprintf(text, 63, L"%hs, %s", hk, d->errorText);
            text[63] = L'\0';
        } else {
            SetHotkeyText(d, row, TRUE, text, 64);
        }
        out->role = ROLE_SYSTEM_PUSHBUTTON;
        _snwprintf(out->name, 159, L"%s hotkey: %s", r->label, text);
        wcscpy(out->action, L"Change");
        break;
    }
    default:
        break;
    }
}

static int SetA11yFocused(void *ctx)
{
    SetEditData *d = (SetEditData *)ctx;
    return SetModelFromRow(d, d->focusRow);
}

/* Posted to the window, so an action requested through MSAA (inside a COM
   call) runs after the call has returned; Save destroys the window.
   wParam = row index. */
#define WM_SET_ACTIVATE (WM_APP + 1)

static BOOL SetA11yInvoke(void *ctx, int index)
{
    SetEditData *d = (SetEditData *)ctx;
    int row = SetRowFromModel(d, index);
    if (row < 0 || !g_setHwnd)
        return FALSE;
    if (row < d->rowCount && d->rows[row].kind == SET_NUMBER)
        return FALSE;   /* a number has no default action, only a value */
    if (row < d->rowCount && !SetRowEnabled(d, &d->rows[row]))
        return FALSE;   /* greyed out: report the refusal instead of a silent no-op */
    PostMessageW(g_setHwnd, WM_SET_ACTIVATE, (WPARAM)row, 0);
    return TRUE;
}

static const A11yModel g_setModel = {
    SetA11yCount, SetA11yDescribe, SetA11yFocused, SetA11yInvoke, &g_set
};

/* ---- Editing ---- */

static void SetMoveFocus(HWND hwnd, SetEditData *d, int row)
{
    d->focusRow = row;
    SetEnsureVisible(d, row);
    RenderSettings(hwnd, d);
    A11y_NotifyFocus(hwnd, SetModelFromRow(d, row));
}

/* Next focusable row from 'from' in direction dir, wrapping through Cancel and Save. */
static int SetStepFocus(SetEditData *d, int from, int dir)
{
    int n = SET_SAVE(d) + 1;
    for (int k = 0; k < n; k++) {
        from = (from + dir + n) % n;
        if (SetRowFocusable(d, from))
            return from;
    }
    return SET_SAVE(d);
}

static void SetCancel(HWND hwnd)
{
    DestroyWindow(hwnd);   /* edits live in the working copy and are dropped */
    g_setHwnd = NULL;
}

static void SetRowChanged(HWND hwnd, SetEditData *d, int row)
{
    RenderSettings(hwnd, d);
    int index = SetModelFromRow(d, row);
    SetRow *r = &d->rows[row];
    if (r->kind == SET_TOGGLE)       A11y_NotifyState(hwnd, index);
    else if (r->kind == SET_NUMBER)  A11y_NotifyValue(hwnd, index);
    else                             A11y_NotifyName(hwnd, index);
}

/* Auto brightness flipped: the rows that depend on it changed their state,
   which a screen reader hears only if told. */
static void SetDependentsChanged(HWND hwnd, SetEditData *d)
{
    for (int i = 0; i < d->rowCount; i++)
        if (d->rows[i].when == SET_WHEN_NO_AUTO)
            A11y_NotifyState(hwnd, SetModelFromRow(d, i));
}

static void SetBeginCapture(HWND hwnd, SetEditData *d, int row)
{
    d->captureRow = row;
    if (d->errorRow == row)
        d->errorRow = -1;
    /* Release our own hotkeys, or pressing the current combination would run
       its action instead of reaching this window. */
    if (g_hotkeyHost) g_hotkeyHost->suspend(TRUE);
    SetRowChanged(hwnd, d, row);
}

static void SetEndCapture(HWND hwnd, SetEditData *d)
{
    int row = d->captureRow;
    if (row < 0)
        return;
    d->captureRow = -1;
    if (g_hotkeyHost) g_hotkeyHost->suspend(FALSE);
    if (hwnd)
        SetRowChanged(hwnd, d, row);
}

static void SetShowError(HWND hwnd, SetEditData *d, int row, const WCHAR *text)
{
    d->errorRow = row;
    wcsncpy(d->errorText, text, 63);
    d->errorText[63] = L'\0';
    d->focusVisible = TRUE;
    SetMoveFocus(hwnd, d, row);
}

static BOOL IsModifierKey(WPARAM vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
    case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        return TRUE;
    default:
        return FALSE;
    }
}

/* A key arrived while a hotkey row is capturing. Modifiers alone keep waiting,
   Escape cancels and Backspace clears, both only without modifiers so that
   Ctrl+Alt+Backspace can still be chosen. */
static void SetCaptureKey(HWND hwnd, SetEditData *d, WPARAM vk)
{
    if (IsModifierKey(vk))
        return;
    /* Whatever happens below ends the capture with this key. Its auto-repeat
       must not then act as a normal key: a held Escape would close the window
       and throw the edits away. */
    d->captureEndVk = vk;

    unsigned mods = 0;
    if (KEY_DOWN(VK_CONTROL))                     mods |= HK_MOD_CONTROL;
    if (KEY_DOWN(VK_MENU))                        mods |= HK_MOD_ALT;
    if (KEY_DOWN(VK_SHIFT))                       mods |= HK_MOD_SHIFT;
    if (KEY_DOWN(VK_LWIN) || KEY_DOWN(VK_RWIN))   mods |= HK_MOD_WIN;

    int row = d->captureRow;
    Hotkey *target = d->rows[row].hval;

    if (mods == 0 && vk == VK_ESCAPE) {
        SetEndCapture(hwnd, d);
        return;
    }
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) {
        target->mods = 0;
        target->vk = 0;
        SetEndCapture(hwnd, d);
        return;
    }

    Hotkey hk = { mods, (unsigned)vk };
    if (!Hotkey_KeyName(hk.vk)) {
        SetEndCapture(NULL, d);
        SetShowError(hwnd, d, row, L"Key not supported");
        return;
    }
    if (!Hotkey_IsValid(hk)) {
        SetEndCapture(NULL, d);
        SetShowError(hwnd, d, row, L"Add Ctrl, Alt or Win");
        return;
    }
    *target = hk;
    SetEndCapture(hwnd, d);
}

/* Copy the working values back into Settings. Only the fields this window owns
   are touched; the schedule points stay with the schedule editor. */
static void SetCommit(SetEditData *d)
{
    Settings *s = d->settings;
    s->step            = d->step;
    s->autostart       = d->autostart;
    s->scheduleEnabled = d->scheduleEnabled;
    s->idleDimEnabled  = d->idleDimEnabled;
    s->idleDimPercent  = d->idleDimPercent;
    s->idleDimMinutes  = d->idleDimMinutes;
    for (int i = 0; i < d->presetCount && i < s->presetCount; i++)
        s->presets[i].brightness = (DWORD)d->presetValues[i];
    for (int i = 0; i < HOTKEY_COUNT; i++)
        s->hotkeys[i] = d->hotkeys[i];
    s->haAutoEnabled = d->haAutoEnabled;
    if (d->haResetCurve)
        s->haCurve.count = 0;   /* back to the default curve */
}

static int SetRowOfHotkey(SetEditData *d, int action)
{
    for (int i = 0; i < d->rowCount; i++)
        if (d->rows[i].kind == SET_HOTKEY && d->rows[i].hval == &d->hotkeys[action])
            return i;
    return -1;
}

/* Save only when every hotkey can be registered. A duplicate inside the set
   or a combination another program holds keeps the window open, with the
   reason on the row and the focus moved there so a screen reader reads it. */
static void SetTrySave(HWND hwnd, SetEditData *d)
{
    SetEndCapture(hwnd, d);

    for (int i = 0; i < HOTKEY_COUNT; i++) {
        for (int j = i + 1; j < HOTKEY_COUNT; j++) {
            if (d->hotkeys[j].vk && Hotkey_Equal(d->hotkeys[i], d->hotkeys[j])) {
                WCHAR msg[64];
                int rowI = SetRowOfHotkey(d, i);
                _snwprintf(msg, 63, L"Same as %s", rowI >= 0 ? d->rows[rowI].label : L"another");
                msg[63] = L'\0';
                SetShowError(hwnd, d, SetRowOfHotkey(d, j), msg);
                return;
            }
        }
    }

    /* Applied even when nothing changed: a hotkey that another program held
       at startup gets another chance, and the user hears if it still fails. */
    if (g_hotkeyHost) {
        int failed = g_hotkeyHost->apply(d->hotkeys);
        if (failed >= 0) {
            SetShowError(hwnd, d, SetRowOfHotkey(d, failed), L"In use by another app");
            return;
        }
    }

    SetCommit(d);
    HWND owner = d->owner;
    DestroyWindow(hwnd);
    g_setHwnd = NULL;
    PostMessageW(owner, WM_COMMAND, (WPARAM)IDM_SETTINGS_SAVED, 0);
}

static void SetRunAction(HWND hwnd, SetEditData *d, int row)
{
    SetRow *r = &d->rows[row];
    if (r->action == SET_ACT_HASS) {
        /* Owned by this window, so this window stays open behind it. */
        UI_ShowHomeAssistant(hwnd, d->settings, d->owner);
    } else if (r->action == SET_ACT_RESET_CURVE && d->settings->haCurve.count > 0) {
        d->haResetCurve = !d->haResetCurve;
        SetRowChanged(hwnd, d, row);
    }
}

/* Activate a row the way a click on its control would. */
static void SetActivate(HWND hwnd, SetEditData *d, int row)
{
    if (row == SET_SAVE(d)) {
        SetTrySave(hwnd, d);
        return;
    }
    if (row == SET_CANCEL(d)) {
        SetCancel(hwnd);
        return;
    }
    SetRow *r = &d->rows[row];
    if (!SetRowEnabled(d, r))
        return;
    if (r->kind == SET_TOGGLE && r->bval) {
        *r->bval = !*r->bval;
        SetRowChanged(hwnd, d, row);
        if (r->bval == &d->haAutoEnabled)
            SetDependentsChanged(hwnd, d);
    } else if (r->kind == SET_HOTKEY) {
        SetBeginCapture(hwnd, d, row);
    } else if (r->kind == SET_ACTION) {
        SetRunAction(hwnd, d, row);
    }
}

/* Keys: Tab, Shift+Tab, Up and Down move between rows and the Save button.
   Left and Right change a number or flip a switch, Space activates the row,
   Enter activates a switch or a hotkey row and saves from anywhere else,
   and Escape closes without saving. */
static void SetKeyDown(HWND hwnd, SetEditData *d, WPARAM vk)
{
    if (d->captureRow >= 0) {
        SetCaptureKey(hwnd, d, vk);
        return;
    }
    if (!d->focusVisible) {
        d->focusVisible = TRUE;
        RenderSettings(hwnd, d);
    }

    int row = d->focusRow;
    SetRow *r = (row < d->rowCount) ? &d->rows[row] : NULL;

    switch (vk) {
    case VK_ESCAPE:
        SetCancel(hwnd);
        return;
    case VK_TAB:
        SetMoveFocus(hwnd, d, SetStepFocus(d, row, KEY_DOWN(VK_SHIFT) ? -1 : 1));
        return;
    case VK_DOWN:
        SetMoveFocus(hwnd, d, SetStepFocus(d, row, 1));
        return;
    case VK_UP:
        SetMoveFocus(hwnd, d, SetStepFocus(d, row, -1));
        return;
    case VK_LEFT:
    case VK_RIGHT: {
        int dir = (vk == VK_RIGHT) ? 1 : -1;
        if (r && r->kind == SET_NUMBER) {
            SetAdjust(r, dir);
            SetRowChanged(hwnd, d, row);
        } else if (r && r->kind == SET_TOGGLE && r->bval && *r->bval != (dir > 0) &&
                   SetRowEnabled(d, r)) {
            *r->bval = (dir > 0);
            SetRowChanged(hwnd, d, row);
            if (r->bval == &d->haAutoEnabled)
                SetDependentsChanged(hwnd, d);
        }
        return;
    }
    case VK_SPACE:
        SetActivate(hwnd, d, row);
        return;
    case VK_RETURN:
        /* A switch, a hotkey row or a button does its own thing; from a number
           row Enter saves, as the default button of a dialog would. */
        if (row == SET_CANCEL(d) ||
            (r && (r->kind == SET_TOGGLE || r->kind == SET_HOTKEY || r->kind == SET_ACTION)))
            SetActivate(hwnd, d, row);
        else
            SetTrySave(hwnd, d);
        return;
    default:
        return;
    }
}

static LRESULT CALLBACK SetWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SetEditData *d = &g_set;

    switch (msg) {
    case WM_GETOBJECT: {
        LRESULT r;
        if (A11y_HandleGetObject(hwnd, wParam, lParam, &r))
            return r;
        break;
    }

    case WM_KEYDOWN: {
        if ((lParam & 0x40000000) && wParam == d->captureEndVk)
            return 0;   /* repeat of the key that started or ended a capture */
        if ((lParam & 0x40000000) && d->captureRow < 0 &&
            (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_SPACE))
            return 0;   /* hold-to-repeat must not save, close or flip twice */
        BOOL wasCapturing = (d->captureRow >= 0);
        SetKeyDown(hwnd, d, wParam);
        /* Enter or Space just opened a capture. Held a little too long, its
           repeat would arrive as the "new hotkey" and end the capture at once. */
        if (!wasCapturing && d->captureRow >= 0)
            d->captureEndVk = wParam;
        return 0;
    }

    case WM_SYSKEYDOWN:
        /* Alt combinations arrive here. While capturing they are the new
           hotkey; otherwise Alt+F4 and friends keep their default meaning. */
        if ((lParam & 0x40000000) && wParam == d->captureEndVk)
            return 0;   /* a held Alt+F4 that was just recorded must not close */
        if (d->captureRow >= 0) {
            SetCaptureKey(hwnd, d, wParam);
            return 0;
        }
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wParam == d->captureEndVk)
            d->captureEndVk = 0;
        break;

    case WM_SET_ACTIVATE:
        /* A screen reader acted on another row: an open capture ends, as it
           does when the mouse clicks elsewhere, so the hotkeys come back. */
        if (d->captureRow >= 0 && d->captureRow != (int)wParam)
            SetEndCapture(hwnd, d);
        if ((int)wParam <= SET_SAVE(d)) {
            d->focusRow = (int)wParam;
            SetActivate(hwnd, d, (int)wParam);
        }
        return 0;

    case WM_SYSCHAR:
        /* Alt+letter looks for a menu mnemonic and beeps when there is none.
           Alt+F4 still works: it arrives as WM_SYSKEYDOWN, not as a character. */
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        RECT rcCancel, rcSave;
        SetButtonRects(d, &rcCancel, &rcSave);
        if (x >= rcSave.left && x <= rcSave.right && y >= rcSave.top && y <= rcSave.bottom) {
            SetTrySave(hwnd, d);
            return 0;
        }
        if (x >= rcCancel.left && x <= rcCancel.right && y >= rcCancel.top && y <= rcCancel.bottom) {
            SetCancel(hwnd);
            return 0;
        }

        RECT rcTrack, rcThumb;
        if (SetScrollbarRects(d, &rcTrack, &rcThumb) && x >= SET_WIDTH - SET_SCROLLBAR_W &&
            y >= rcTrack.top && y < rcTrack.bottom) {
            if (y < rcThumb.top || y >= rcThumb.bottom)   /* the track pages, as in Windows */
                SetScrollTo(d, d->scroll + (y < rcThumb.top ? -d->viewH : d->viewH) * 9 / 10);
            d->thumbDrag = TRUE;
            d->thumbDragY = y;
            d->thumbDragScroll = d->scroll;
            SetCapture(hwnd);
            RenderSettings(hwnd, d);
            return 0;
        }

        int hit;
        int row = SetHitTest(d, x, y, &hit);
        if (d->captureRow >= 0 && !(row == d->captureRow && hit == SETHIT_HOTKEY))
            SetEndCapture(hwnd, d);   /* a click elsewhere cancels the capture */
        if (row >= 0) {
            SetRow *r = &d->rows[row];
            d->focusRow = row;
            d->hoverRow = row;
            if (hit == SETHIT_TOGGLE && r->bval) {
                *r->bval = !*r->bval;
                SetRowChanged(hwnd, d, row);
                if (r->bval == &d->haAutoEnabled)
                    SetDependentsChanged(hwnd, d);
            } else if (hit == SETHIT_MINUS || hit == SETHIT_PLUS) {
                SetAdjust(r, hit == SETHIT_PLUS ? 1 : -1);
                SetRowChanged(hwnd, d, row);
            } else if (hit == SETHIT_HOTKEY && d->captureRow != row) {
                SetBeginCapture(hwnd, d, row);
            } else if (hit == SETHIT_ACTION) {
                RenderSettings(hwnd, d);
                SetRunAction(hwnd, d, row);
            } else if (hit == SETHIT_ROW) {
                RenderSettings(hwnd, d);
                BeginWindowDrag(hwnd);   /* the label is not a control: it moves the window */
            } else {
                RenderSettings(hwnd, d);
            }
        } else {
            BeginWindowDrag(hwnd);       /* header, section titles, footer */
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (d->thumbDrag)
            ReleaseCapture();   /* WM_CAPTURECHANGED ends the drag */
        return 0;

    case WM_CAPTURECHANGED:
        if (d->thumbDrag) {
            d->thumbDrag = FALSE;
            RenderSettings(hwnd, d);
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (d->thumbDrag) {
            RECT rcTrack, rcThumb;
            if (SetScrollbarRects(d, &rcTrack, &rcThumb)) {
                int travel = (rcTrack.bottom - rcTrack.top) - (rcThumb.bottom - rcThumb.top);
                int dy = GET_Y_LPARAM(lParam) - d->thumbDragY;
                if (travel > 0)
                    SetScrollTo(d, d->thumbDragScroll + dy * SetMaxScroll(d) / travel);
                RenderSettings(hwnd, d);
            }
            return 0;
        }
        int hit;
        int row = SetHitTest(d, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &hit);
        if (row >= 0 && !SetRowEnabled(d, &d->rows[row]))
            row = -1;   /* a greyed-out row does not light up */
        if (row != d->hoverRow) {
            d->hoverRow = row;
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        static int wheelAccum = 0;
        int notches = WheelNotches(&wheelAccum, wParam);
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        if (notches == 0)
            return 0;
        int hit;
        int row = SetHitTest(d, pt.x, pt.y, &hit);
        BOOL scrollable = SetMaxScroll(d) > 0;
        BOOL latched = scrollable && GetTickCount() - d->lastWheelScroll < SET_WHEEL_LATCH_MS;
        if (!latched && row >= 0 && d->rows[row].kind == SET_NUMBER &&
            SetRowEnabled(d, &d->rows[row]) && SetOverNumberControl(d, row, pt.x, pt.y)) {
            for (int n = notches; n != 0; n += (n > 0 ? -1 : 1))
                SetAdjust(&d->rows[row], n > 0 ? 1 : -1);
            d->hoverRow = row;
            SetRowChanged(hwnd, d, row);
        } else if (scrollable) {
            /* Anywhere but over a number control the wheel scrolls, two rows a
               notch. The time is taken even at either end, so a wheel still
               spinning there does not start changing a value. */
            SetScrollTo(d, d->scroll - notches * 2 * SET_ROW_H);
            d->lastWheelScroll = GetTickCount();
            d->hoverRow = SetHitTest(d, pt.x, pt.y, &hit);
            RenderSettings(hwnd, d);
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* Dismiss without saving on click-outside, like the schedule editor.
               The Home Assistant window is opened from here and is owned by
               this one, so losing the focus to it (or to the browser the user
               copies a token from while it is open) keeps this window. */
            HWND ha = UI_HassWindow();
            if (ha && ((HWND)lParam == ha || IsWindowVisible(ha)))
                return 0;
            DestroyWindow(hwnd);
            g_setHwnd = NULL;
        } else {
            RenderSettings(hwnd, d);   /* the sensor may have changed in the HA window */
            SetDependentsChanged(hwnd, d);   /* and with it whether the schedule is greyed out */
        }
        return 0;

    case WM_DESTROY:
        SetEndCapture(NULL, d);   /* give the hotkeys back if a capture was open */
        A11y_Detach(hwnd);
        g_setHwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void UI_ShowSettings(HWND hwndOwner, Settings *s)
{
    if (g_setHwnd && IsWindow(g_setHwnd)) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }

    memset(&g_set, 0, sizeof(g_set));
    g_set.settings = s;
    g_set.owner = hwndOwner;
    g_set.hoverRow = -1;
    g_set.captureRow = -1;
    g_set.errorRow = -1;
    g_set.step = s->step;
    g_set.autostart = Settings_GetAutostart();   /* the registry is the truth here */
    g_set.scheduleEnabled = s->scheduleEnabled;
    g_set.idleDimEnabled = s->idleDimEnabled;
    g_set.idleDimPercent = s->idleDimPercent;
    g_set.idleDimMinutes = s->idleDimMinutes;
    g_set.presetCount = s->presetCount;
    for (int i = 0; i < s->presetCount; i++)
        g_set.presetValues[i] = (int)s->presets[i].brightness;
    for (int i = 0; i < HOTKEY_COUNT; i++)
        g_set.hotkeys[i] = s->hotkeys[i];
    g_set.haAutoEnabled = s->haAutoEnabled;
    BuildSettingsRows(&g_set);
    g_set.focusRow = SetStepFocus(&g_set, SET_SAVE(&g_set), 1);   /* first editable row */
    if (g_hotkeyHost && g_hotkeyHost->firstFailed() >= 0) {
        g_set.errorRow = SetRowOfHotkey(&g_set, g_hotkeyHost->firstFailed());
        wcscpy(g_set.errorText, L"In use by another app");
    }

    /* Centered on the monitor the cursor is on. The window is too tall to
       hang off the cursor the way the menu does: opened from the tray menu it
       was pushed into the top corner of the screen. */
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    /* The rows get what the work area leaves after the header, the footer and
       a margin; when that is less than they need, they scroll. */
    int room = (mi.rcWork.bottom - mi.rcWork.top) - SET_HEADER_H - SET_FOOTER_H - 2 * 8;
    int content = SetContentHeight(&g_set);
    g_set.viewH = (room < content) ? (room > 3 * SET_ROW_H ? room : 3 * SET_ROW_H) : content;
    g_set.scroll = 0;
    g_set.thumbDrag = FALSE;
    g_set.lastWheelScroll = GetTickCount() - SET_WHEEL_LATCH_MS;

    int w = SET_WIDTH;
    int h = SetHeight(&g_set);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2;
    int y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    g_setHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        SET_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_uiInst, NULL);
    if (!g_setHwnd) return;
    A11y_Attach(g_setHwnd, &g_setModel);

    RenderSettings(g_setHwnd, &g_set);
    ShowWindow(g_setHwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_setHwnd);
    A11y_NotifyFocus(g_setHwnd, SetModelFromRow(&g_set, g_set.focusRow));
}

/* ---- Class registration ---- */

void UiSettings_Init(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SetWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SET_CLASS;
    RegisterClassExW(&wc);
}

void UiSettings_Shutdown(void)
{
    if (g_setHwnd) {
        DestroyWindow(g_setHwnd);
        g_setHwnd = NULL;
    }
}
