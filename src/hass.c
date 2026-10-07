#include "hass.h"
#include "hassurl.h"
#include "json.h"
#include <winhttp.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HASS_TIMEOUT_MS   8000
#define HASS_BODY_MAX     (256 * 1024)   /* a reply larger than this is not one of ours */
#define HASS_MAX_TOKENS   4096

/* One line per illuminance sensor: entity, name, area, state, separated by
   tabs. area_name() falls back to the device's area. attributes.get() keeps
   sensors without a device_class from failing the render. Names and areas
   lose their tabs and line breaks, which would shift the fields. */
static const char kListTemplate[] =
    "{%- for s in states.sensor if s.attributes.get('device_class') == 'illuminance' -%}"
    "{{ s.entity_id }}{{ '\\t' }}{{ s.name | replace('\\t', ' ') | replace('\\n', ' ') }}"
    "{{ '\\t' }}{{ (area_name(s.entity_id) or '') | replace('\\t', ' ') | replace('\\n', ' ') }}"
    "{{ '\\t' }}{{ s.state }}{{ '\\n' }}"
    "{%- endfor -%}";

/* WebSocket messages are read in pieces; a reply in more pieces than this is
   not one of ours (a broken or hostile server sending empty fragments). */
#define HASS_MAX_FRAGMENTS 1024

/* ---- Connection ---- */

typedef struct {
    HINTERNET session;
    HINTERNET connect;
    WCHAR     basePath[HASS_URL_MAX];   /* path of the URL without a trailing slash */
    BOOL      secure;
} HassConn;

static void CloseConn(HassConn *c)
{
    if (c->connect) WinHttpCloseHandle(c->connect);
    if (c->session) WinHttpCloseHandle(c->session);
    c->connect = c->session = NULL;
}

static HassStatus OpenConn(HassConn *c, const WCHAR *url)
{
    memset(c, 0, sizeof(*c));
    WCHAR host[HASS_URL_MAX], path[HASS_URL_MAX];
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.lpszHostName = host;  uc.dwHostNameLength = HASS_URL_MAX;
    uc.lpszUrlPath = path;   uc.dwUrlPathLength = HASS_URL_MAX;
    /* config.ini may hold an address typed by hand, without a scheme. */
    WCHAR full[HASS_URL_MAX];
    if (!url || !HassUrl_Normalize(url, full, HASS_URL_MAX) ||
        !WinHttpCrackUrl(full, 0, 0, &uc) || !host[0])
        return HASS_ERR_URL;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
        return HASS_ERR_URL;
    c->secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    wcsncpy(c->basePath, path, HASS_URL_MAX - 1);
    size_t n = wcslen(c->basePath);
    while (n > 0 && c->basePath[n - 1] == L'/')
        c->basePath[--n] = L'\0';

    c->session = WinHttpOpen(L"Lumos", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session)
        return HASS_ERR_CONNECT;
    WinHttpSetTimeouts(c->session, HASS_TIMEOUT_MS, HASS_TIMEOUT_MS, HASS_TIMEOUT_MS, HASS_TIMEOUT_MS);
    /* The token is attached as a header we add ourselves, so a redirect could
       carry it to another host. Home Assistant has no reason to redirect an
       API call; a 3xx is reported as an error instead. */
    DWORD noRedirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(c->session, WINHTTP_OPTION_REDIRECT_POLICY, &noRedirect, sizeof(noRedirect));
    c->connect = WinHttpConnect(c->session, host, uc.nPort, 0);
    if (!c->connect) {
        CloseConn(c);
        return HASS_ERR_CONNECT;
    }
    return HASS_OK;
}

static HassStatus StatusFromError(DWORD err)
{
    switch (err) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return HASS_ERR_RESOLVE;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        return HASS_ERR_TLS;
    default:
        return HASS_ERR_CONNECT;
    }
}

static HassStatus StatusFromHttp(DWORD code)
{
    if (code == 401 || code == 403) return HASS_ERR_AUTH;
    if (code == 404) return HASS_ERR_NOT_FOUND;
    return HASS_ERR_HTTP;
}

/* Open a GET request for basePath + suffix with the token attached. */
static HINTERNET OpenGet(HassConn *c, const WCHAR *suffix, const char *token)
{
    WCHAR path[HASS_URL_MAX + HASS_ENTITY_MAX + 32];
    _snwprintf(path, sizeof(path) / sizeof(path[0]) - 1, L"%s%s", c->basePath, suffix);
    path[sizeof(path) / sizeof(path[0]) - 1] = L'\0';
    HINTERNET req = WinHttpOpenRequest(c->connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       c->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        return NULL;
    WCHAR header[HASS_TOKEN_MAX + 64];
    int n = _snwprintf(header, sizeof(header) / sizeof(header[0]) - 1,
                       L"Authorization: Bearer %hs\r\n", token);
    BOOL added = (n >= 0) && WinHttpAddRequestHeaders(req, header, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    SecureZeroMemory(header, sizeof(header));
    if (!added) {
        WinHttpCloseHandle(req);
        return NULL;
    }
    return req;
}

/* Send the request and read the status code. */
static HassStatus SendAndReceive(HINTERNET req, DWORD *code)
{
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        return StatusFromError(GetLastError());
    DWORD size = sizeof(*code);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, code, &size, WINHTTP_NO_HEADER_INDEX))
        return HASS_ERR_PROTOCOL;
    return HASS_OK;
}

/* Read the whole body into a malloc'd, NUL-terminated buffer. */
static char *ReadBody(HINTERNET req, int *len)
{
    int cap = 4096, n = 0;
    char *buf = (char *)malloc((size_t)cap);
    for (;;) {
        if (!buf) return NULL;
        if (n + 2048 + 1 > cap) {
            if (cap >= HASS_BODY_MAX) { free(buf); return NULL; }
            char *bigger = (char *)realloc(buf, (size_t)cap * 2);
            if (!bigger) { free(buf); return NULL; }
            buf = bigger;
            cap *= 2;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + n, 2048, &got)) { free(buf); return NULL; }
        if (got == 0) break;
        n += (int)got;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* ---- REST: one sensor ---- */

/* A numeric state such as "5" or "12.5". "unavailable", "unknown" and text
   are not readings. */
static BOOL ParseLux(const char *s, double *lux)
{
    char *end;
    double v = strtod(s, &end);
    /* "inf", "1e999" and NaN are not light levels; neither is anything above
       a million lux (direct sunlight is about 100 000). */
    if (end == s || *end != '\0' || !isfinite(v) || v < 0 || v > 1e6)
        return FALSE;
    *lux = v;
    return TRUE;
}

HassStatus Hass_ReadLux(const WCHAR *url, const char *token, const WCHAR *entityId,
                        double *lux, BOOL *hasValue)
{
    *hasValue = FALSE;
    HassConn c;
    HassStatus st = OpenConn(&c, url);
    if (st != HASS_OK)
        return st;
    WCHAR suffix[HASS_ENTITY_MAX + 16];
    _snwprintf(suffix, sizeof(suffix) / sizeof(suffix[0]) - 1, L"/api/states/%s", entityId);
    suffix[sizeof(suffix) / sizeof(suffix[0]) - 1] = L'\0';

    HINTERNET req = OpenGet(&c, suffix, token);
    if (!req) {
        CloseConn(&c);
        return HASS_ERR_CONNECT;
    }
    DWORD code = 0;
    st = SendAndReceive(req, &code);
    if (st == HASS_OK && code != 200)
        st = StatusFromHttp(code);
    if (st == HASS_OK) {
        int len = 0;
        char *body = ReadBody(req, &len);
        st = body ? HASS_ERR_PROTOCOL : HASS_ERR_CONNECT;   /* no body: the connection broke */
        if (body) {
            /* Per call, so threads never share it; 1024 tokens leave room for a
               sensor with many attributes. */
            JsonToken *tok = (JsonToken *)malloc(sizeof(JsonToken) * 1024);
            int n = tok ? Json_Parse(body, len, tok, 1024) : -1;
            int v = (n > 0) ? Json_Get(body, tok, n, 0, "state") : -1;
            char state[64];
            if (v >= 0 && Json_GetString(body, &tok[v], state, sizeof state) >= 0) {
                *hasValue = ParseLux(state, lux);
                st = HASS_OK;
            }
            free(tok);
            free(body);
        }
    }
    WinHttpCloseHandle(req);
    CloseConn(&c);
    return st;
}

/* ---- WebSocket: the sensor list ---- */

/* Receive one complete text message into a malloc'd, NUL-terminated buffer. */
static char *WsReceive(HINTERNET ws, int *len)
{
    int cap = 8192, n = 0, fragments = 0;
    char *buf = (char *)malloc((size_t)cap);
    for (;;) {
        if (!buf) return NULL;
        if (++fragments > HASS_MAX_FRAGMENTS) { free(buf); return NULL; }
        if (n + 4096 + 1 > cap) {
            if (cap >= HASS_BODY_MAX) { free(buf); return NULL; }
            char *bigger = (char *)realloc(buf, (size_t)cap * 2);
            if (!bigger) { free(buf); return NULL; }
            buf = bigger;
            cap *= 2;
        }
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(ws, buf + n, 4096, &got, &type) != ERROR_SUCCESS) {
            free(buf);
            return NULL;
        }
        n += (int)got;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            break;
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {   /* binary or close */
            free(buf);
            return NULL;
        }
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static BOOL WsSend(HINTERNET ws, const char *text)
{
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                (PVOID)text, (DWORD)strlen(text)) == ERROR_SUCCESS;
}

/* One WebSocket message with its tokens. The tokens live per call, since
   the poll thread and the HA window may both talk to HA at the same time. */
typedef struct {
    char      *text;
    int        len;
    JsonToken *tok;    /* HASS_MAX_TOKENS entries, owned by the caller */
    int        count;
    char       type[32];
} WsMessage;

/* Receive the next message into m and read its "type" ("" when there is none).
   Returns FALSE when nothing could be received. */
static BOOL WsReceiveTyped(HINTERNET ws, WsMessage *m)
{
    free(m->text);
    m->type[0] = '\0';
    m->text = WsReceive(ws, &m->len);
    if (!m->text)
        return FALSE;
    m->count = Json_Parse(m->text, m->len, m->tok, HASS_MAX_TOKENS);
    int t = (m->count > 0) ? Json_Get(m->text, m->tok, m->count, 0, "type") : -1;
    if (t < 0 || Json_GetString(m->text, &m->tok[t], m->type, sizeof m->type) < 0)
        m->type[0] = '\0';
    return TRUE;
}

/* Convert len bytes of UTF-8. A text longer than the buffer is cut to fit
   (MultiByteToWideChar alone would return nothing at all). Returns FALSE
   when nothing could be converted or the text had to be cut. */
static BOOL Utf8ToWide(const char *s, int len, WCHAR *out, int cap)
{
    WCHAR tmp[512];
    int n = (len > 0) ? MultiByteToWideChar(CP_UTF8, 0, s, len, tmp, 511) : 0;
    tmp[n > 0 ? n : 0] = L'\0';
    lstrcpynW(out, tmp, cap);
    return n > 0 && n < cap;
}

/* Split the rendered template into sensors. */
static int ParseList(const char *text, HassSensor *out, int max)
{
    int count = 0;
    const char *line = text;
    while (*line && count < max) {
        const char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        const char *f[4];
        int flen[4], nf = 0;
        const char *p = line;
        while (nf < 4) {
            const char *tab = memchr(p, '\t', (size_t)(eol - p));
            const char *fe = (tab && nf < 3) ? tab : eol;
            f[nf] = p;
            flen[nf] = (int)(fe - p);
            nf++;
            if (fe == eol) break;
            p = fe + 1;
        }
        if (nf == 4 && flen[0] > 0) {
            HassSensor *s = &out[count];
            /* An entity id that does not fit cannot be saved and read back,
               so the sensor is left out; a long name or area is just cut. */
            if (Utf8ToWide(f[0], flen[0], s->entityId, HASS_ENTITY_MAX)) {
                count++;
                Utf8ToWide(f[1], flen[1], s->name, 128);
                Utf8ToWide(f[2], flen[2], s->area, 64);
                char state[64];
                s->hasValue = FALSE;
                if (flen[3] < 64) {   /* a longer state is no number we accept */
                    memcpy(state, f[3], (size_t)flen[3]);
                    state[flen[3]] = '\0';
                    s->hasValue = ParseLux(state, &s->lux);
                }
                if (!s->hasValue) s->lux = 0;
            }
        }
        line = *eol ? eol + 1 : eol;
    }
    return count;
}

HassStatus Hass_ListSensors(const WCHAR *url, const char *token,
                            HassSensor *out, int max, int *count)
{
    *count = 0;
    HassConn c;
    HassStatus st = OpenConn(&c, url);
    if (st != HASS_OK)
        return st;

    WCHAR path[HASS_URL_MAX + 32];
    _snwprintf(path, sizeof(path) / sizeof(path[0]) - 1, L"%s/api/websocket", c.basePath);
    path[sizeof(path) / sizeof(path[0]) - 1] = L'\0';
    HINTERNET req = WinHttpOpenRequest(c.connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       c.secure ? WINHTTP_FLAG_SECURE : 0);
    HINTERNET ws = NULL;
    WsMessage m = { 0 };
    m.tok = (JsonToken *)malloc(sizeof(JsonToken) * HASS_MAX_TOKENS);
    char *auth = NULL, *cmd = NULL;
    int authCap = 0;

    if (!req || !m.tok) {
        st = HASS_ERR_CONNECT;
        goto done;
    }
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        st = HASS_ERR_PROTOCOL;
        goto done;
    }
    DWORD code = 0;
    st = SendAndReceive(req, &code);
    if (st != HASS_OK)
        goto done;
    if (code != 101) {
        st = StatusFromHttp(code);
        goto done;
    }
    ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (!ws) {
        st = HASS_ERR_PROTOCOL;
        goto done;
    }
    /* Bound every receive on the socket too, so a server that stops talking
       cannot hold the worker. */
    DWORD wsTimeout = HASS_TIMEOUT_MS;
    WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &wsTimeout, sizeof(wsTimeout));

    /* HA greets with auth_required, answers the token with auth_ok or
       auth_invalid, then sends the command result and the first render. */
    st = HASS_ERR_PROTOCOL;
    if (!WsReceiveTyped(ws, &m) || strcmp(m.type, "auth_required") != 0)
        goto done;

    int tokLen = (int)strlen(token);
    authCap = tokLen * 6 + 64;
    auth = (char *)calloc(1, (size_t)authCap);
    if (!auth)
        goto done;
    strcpy(auth, "{\"type\":\"auth\",\"access_token\":");
    if (Json_Quote(token, auth + strlen(auth), tokLen * 6 + 8) < 0)
        goto done;
    strcat(auth, "}");
    BOOL sent = WsSend(ws, auth);
    SecureZeroMemory(auth, strlen(auth));
    if (!sent)
        goto done;

    if (!WsReceiveTyped(ws, &m))
        goto done;
    if (strcmp(m.type, "auth_invalid") == 0) {
        st = HASS_ERR_AUTH;
        goto done;
    }
    if (strcmp(m.type, "auth_ok") != 0)
        goto done;

    cmd = (char *)malloc(sizeof(kListTemplate) * 2 + 128);
    if (!cmd)
        goto done;
    strcpy(cmd, "{\"id\":1,\"type\":\"render_template\",\"template\":");
    if (Json_Quote(kListTemplate, cmd + strlen(cmd), (int)sizeof(kListTemplate) * 2 + 64) < 0)
        goto done;
    strcat(cmd, "}");
    if (!WsSend(ws, cmd))
        goto done;

    /* The result message only acknowledges the subscription; the rendered
       text arrives in the first event. */
    for (int i = 0; i < 4; i++) {
        if (!WsReceiveTyped(ws, &m))
            goto done;
        /* Only replies to our command (id 1) count. */
        int id = Json_Get(m.text, m.tok, m.count, 0, "id");
        if (id < 0 || m.tok[id].type != JSON_PRIMITIVE || m.tok[id].end - m.tok[id].start != 1 ||
            m.text[m.tok[id].start] != '1')
            continue;
        if (strcmp(m.type, "result") == 0) {
            int s = Json_Get(m.text, m.tok, m.count, 0, "success");
            if (s < 0 || !Json_IsTrue(m.text, &m.tok[s])) {
                /* A refused command is a permission problem, not a reply
                   Lumos does not understand. */
                int err = Json_Get(m.text, m.tok, m.count, 0, "error");
                int code = Json_Get(m.text, m.tok, m.count, err, "code");
                if (code >= 0 && Json_IsString(m.text, &m.tok[code], "unauthorized"))
                    st = HASS_ERR_AUTH;
                goto done;
            }
            continue;
        }
        if (strcmp(m.type, "event") == 0) {
            int ev = Json_Get(m.text, m.tok, m.count, 0, "event");
            int r = Json_Get(m.text, m.tok, m.count, ev, "result");
            if (r < 0)
                goto done;   /* a render error */
            /* Decoded UTF-8 is never longer than the escaped text. */
            int cap = m.tok[r].end - m.tok[r].start + 1;
            char *text = (char *)malloc((size_t)cap);
            if (!text)
                goto done;
            if (Json_GetString(m.text, &m.tok[r], text, cap) >= 0) {
                *count = ParseList(text, out, max);
                st = HASS_OK;
            }
            free(text);
            goto done;
        }
    }

done:
    free(m.text);
    free(m.tok);
    if (auth)
        SecureZeroMemory(auth, (size_t)authCap);   /* also when a step before the send failed */
    free(auth);
    free(cmd);
    if (ws) {
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(ws);
    }
    if (req) WinHttpCloseHandle(req);
    CloseConn(&c);
    return st;
}

const WCHAR *Hass_StatusText(HassStatus status)
{
    switch (status) {
    case HASS_OK:            return L"Connected.";
    case HASS_ERR_URL:       return L"The URL is not valid. Example: http://192.168.1.10:8123";
    case HASS_ERR_RESOLVE:   return L"The host name was not found.";
    case HASS_ERR_CONNECT:   return L"Home Assistant did not answer. Check the URL, and the VPN when you are away.";
    /* Common on a home network: an https certificate issued for a name,
       reached through the IP address. Validation is never turned off. */
    case HASS_ERR_TLS:       return L"Certificate not accepted. Use the name it was issued for, or http:// on the LAN.";
    /* A wrong token and a local-only user outside the home network both come
       back as 401, so one message covers both. */
    case HASS_ERR_AUTH:      return L"Token refused. Check it, and that this PC is on the home network or the VPN.";
    case HASS_ERR_NOT_FOUND: return L"The sensor was not found in Home Assistant.";
    case HASS_ERR_HTTP:      return L"Home Assistant returned an unexpected error.";
    default:                 return L"Home Assistant sent a reply Lumos did not understand.";
    }
}
