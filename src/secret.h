#ifndef SECRET_H
#define SECRET_H

#include <windows.h>

/* Keeps a secret (the Home Assistant token) in config.ini without writing it
 * in clear. DPAPI encrypts it with a key tied to the Windows user account, so
 * the stored text is useless to another account or another machine, and the
 * result is stored as base64. */

/* Encrypt plain (UTF-8) and write it as base64 into out. An empty plain gives
   an empty out. Returns FALSE when out is too small or DPAPI fails. */
BOOL Secret_Protect(const char *plain, WCHAR *out, int cap);

/* Reverse Secret_Protect. Returns FALSE (and an empty out) when the text
   cannot be decrypted, for example when config.ini came from another account. */
BOOL Secret_Unprotect(const WCHAR *stored, char *out, int cap);

#endif /* SECRET_H */
