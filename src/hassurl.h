#ifndef HASSURL_H
#define HASSURL_H

#include <wchar.h>

/* The Home Assistant address as the user types it. Win32-free, unit-tested
 * in tests/test_hassurl.c.
 *
 * Users on a home network often type "192.168.1.10:8123" with no scheme, or
 * paste the address with a space around it. WinHttpCrackUrl rejects both, so
 * the text is normalized before anything else looks at it. */

/* Trim white space and add "http://" when the text has no scheme, the way a
   browser does (a new Home Assistant answers on http, port 8123). Returns 1
   on success, 0 when the text is empty or the result does not fit in cap. */
int HassUrl_Normalize(const wchar_t *in, wchar_t *out, int cap);

/* 1 when the (normalized) URL uses plain http. */
int HassUrl_IsHttp(const wchar_t *url);

/* 1 when the host of the (normalized) URL is on the local network: a private
   or link-local IPv4 address (10/8, 172.16/12, 192.168/16, 169.254/16), the
   loopback, a unique local or link-local IPv6 address (fc00::/7, fe80::/10,
   ::1), a name ending in .local, .lan, .internal or .home.arpa, or a name
   without a dot. A token sent over http to such a host stays on the LAN. */
int HassUrl_IsLocal(const wchar_t *url);

#endif /* HASSURL_H */
