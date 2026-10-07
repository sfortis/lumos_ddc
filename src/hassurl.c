#include "hassurl.h"
#include <wctype.h>

static int StartsWithNoCase(const wchar_t *s, const wchar_t *prefix)
{
    for (; *prefix; s++, prefix++)
        if (towlower(*s) != towlower(*prefix))
            return 0;
    return 1;
}

static int EndsWithNoCase(const wchar_t *s, int len, const wchar_t *suffix)
{
    int n = (int)wcslen(suffix);
    if (len < n)
        return 0;
    for (int i = 0; i < n; i++)
        if (towlower(s[len - n + i]) != towlower(suffix[i]))
            return 0;
    return 1;
}

/* A scheme is letters before "://", with no '/', '.' or ':' in front of it,
   so "192.168.1.10:8123" and "ha.local:8123/x" have none. */
static int HasScheme(const wchar_t *s)
{
    int i = 0;
    while (iswalpha(s[i]) || (i > 0 && (iswdigit(s[i]) || s[i] == L'+' || s[i] == L'-')))
        i++;
    return i > 0 && s[i] == L':' && s[i + 1] == L'/' && s[i + 2] == L'/';
}

int HassUrl_Normalize(const wchar_t *in, wchar_t *out, int cap)
{
    if (!in || !out || cap <= 0)
        return 0;
    out[0] = L'\0';
    while (iswspace(*in))
        in++;
    int len = (int)wcslen(in);
    while (len > 0 && iswspace(in[len - 1]))
        len--;
    if (len == 0)
        return 0;
    static const wchar_t kDefault[] = L"http://";
    int pre = HasScheme(in) ? 0 : (int)(sizeof(kDefault) / sizeof(kDefault[0])) - 1;
    if (pre + len + 1 > cap)
        return 0;
    for (int i = 0; i < pre; i++)
        out[i] = kDefault[i];
    for (int i = 0; i < len; i++)
        out[pre + i] = in[i];
    out[pre + len] = L'\0';
    return 1;
}

int HassUrl_IsHttp(const wchar_t *url)
{
    return url && StartsWithNoCase(url, L"http://");
}

/* The host part of a URL: after "://" and any "user@", up to the port, the
   path, the query or the fragment. An IPv6 literal is returned without its
   brackets. Returns the length, 0 when there is none. */
static int HostOf(const wchar_t *url, const wchar_t **host)
{
    const wchar_t *p = wcsstr(url, L"://");
    if (!p)
        return 0;
    p += 3;
    /* A backslash ends it too: some parsers read "evil.com\@10.0.0.1" as the
       host evil.com, and the note must not call that local. */
    const wchar_t *end = p;
    while (*end && *end != L'/' && *end != L'\\' && *end != L'?' && *end != L'#')
        end++;
    for (const wchar_t *q = p; q < end; q++)
        if (*q == L'@')
            p = q + 1;
    if (*p == L'[') {
        const wchar_t *close = p + 1;
        while (close < end && *close != L']')
            close++;
        if (close >= end || (close + 1 < end && close[1] != L':'))
            return 0;   /* only a port may follow "]" */
        *host = p + 1;
        return (int)(close - p - 1);
    }
    const wchar_t *colon = p;
    while (colon < end && *colon != L':')
        colon++;
    *host = p;
    return (int)(colon - p);
}

/* Four dotted decimal numbers 0-255, without leading zeros: a resolver may
   read "010" as octal, so "010.0.0.1" is not taken for 10.0.0.1. */
static int ParseIPv4(const wchar_t *s, int len, int octet[4])
{
    int part = 0, value = -1;
    for (int i = 0; i <= len; i++) {
        if (i == len || s[i] == L'.') {
            if (value < 0 || part > 3)
                return 0;
            octet[part++] = value;
            value = -1;
        } else if (iswdigit(s[i])) {
            if (value == 0)
                return 0;   /* a leading zero */
            value = (value < 0 ? 0 : value * 10) + (s[i] - L'0');
            if (value > 255)
                return 0;
        } else {
            return 0;
        }
    }
    return part == 4;
}

/* The first 16-bit group of an IPv6 literal, -1 when it is not one. */
static int FirstIPv6Group(const wchar_t *s, int len)
{
    int colons = 0, digits = 0;
    for (int i = 0; i < len; i++)
        if (s[i] == L'%')
            len = i;   /* a zone id ("%25eth0") follows the address itself */
    for (int i = 0; i < len && s[i] != L':'; i++)
        if (++digits > 4)
            return -1;   /* a group is at most four hex digits */
    for (int i = 0; i < len; i++) {
        if (s[i] == L':')
            colons++;
        else if (!iswxdigit(s[i]) && s[i] != L'.')
            return -1;
    }
    if (colons < 2)
        return -1;
    int v = 0;
    for (int i = 0; i < len && s[i] != L':'; i++)
        v = v * 16 + (iswdigit(s[i]) ? s[i] - L'0' : towlower(s[i]) - L'a' + 10);
    return v;
}

int HassUrl_IsLocal(const wchar_t *url)
{
    const wchar_t *h = 0;
    int len = url ? HostOf(url, &h) : 0;
    if (len <= 0)
        return 0;
    while (len > 0 && h[len - 1] == L'.')
        len--;   /* "ha.local." is a fully qualified "ha.local" */
    if (len == 0)
        return 0;

    int o[4];
    if (ParseIPv4(h, len, o))
        return o[0] == 10 || o[0] == 127 ||
               (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
               (o[0] == 192 && o[1] == 168) ||
               (o[0] == 169 && o[1] == 254);

    int g = FirstIPv6Group(h, len);
    if (g >= 0) {
        if (len == 3 && h[0] == L':' && h[1] == L':' && h[2] == L'1')
            return 1;                        /* ::1 */
        if (h[0] == L':')
            return 0;                        /* other "::" forms are not local */
        return (g & 0xFE00) == 0xFC00 ||     /* fc00::/7, unique local */
               (g & 0xFFC0) == 0xFE80;       /* fe80::/10, link-local */
    }

    int dot = 0;
    for (int i = 0; i < len; i++)
        if (h[i] == L'.')
            dot = 1;
    if (!dot) {
        /* "homeassistant" or "localhost". It must start with a letter and hold
           only letters, digits and '-': "134744072" and "0x08080808" are
           8.8.8.8 to a resolver, and "a%2elocal" is not a plain name. */
        if (!iswalpha(h[0]))
            return 0;
        for (int i = 0; i < len; i++)
            if (!iswalnum(h[i]) && h[i] != L'-')
                return 0;
        return 1;
    }
    return EndsWithNoCase(h, len, L".local") || EndsWithNoCase(h, len, L".lan") ||
           EndsWithNoCase(h, len, L".internal") || EndsWithNoCase(h, len, L".home.arpa");
}
