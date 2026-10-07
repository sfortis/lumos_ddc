#include "secret.h"
#include <wincrypt.h>
#include <string.h>

/* Extra entropy, so another program running as the same user cannot decrypt
   the value by passing it to CryptUnprotectData without knowing this. It is
   not a secret in itself; it only stops a casual reuse of the blob. */
static BYTE kEntropy[] = "Lumos Home Assistant token";

BOOL Secret_Protect(const char *plain, WCHAR *out, int cap)
{
    if (cap <= 0)
        return FALSE;
    out[0] = L'\0';
    if (!plain[0])
        return TRUE;
    DATA_BLOB in = { (DWORD)strlen(plain), (BYTE *)plain };
    DATA_BLOB ent = { sizeof(kEntropy) - 1, kEntropy };
    DATA_BLOB enc = { 0 };
    if (!CryptProtectData(&in, L"Lumos", &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &enc))
        return FALSE;
    DWORD n = (DWORD)cap;
    BOOL ok = CryptBinaryToStringW(enc.pbData, enc.cbData,
                                   CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &n);
    LocalFree(enc.pbData);
    if (!ok)
        out[0] = L'\0';
    return ok;
}

BOOL Secret_Unprotect(const WCHAR *stored, char *out, int cap)
{
    if (cap <= 0)
        return FALSE;
    out[0] = '\0';
    if (!stored[0])
        return TRUE;
    BYTE bin[2048];
    DWORD binLen = sizeof(bin);
    if (!CryptStringToBinaryW(stored, 0, CRYPT_STRING_BASE64, bin, &binLen, NULL, NULL))
        return FALSE;
    DATA_BLOB in = { binLen, bin };
    DATA_BLOB ent = { sizeof(kEntropy) - 1, kEntropy };
    DATA_BLOB dec = { 0 };
    if (!CryptUnprotectData(&in, NULL, &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &dec))
        return FALSE;
    BOOL ok = (int)dec.cbData < cap;
    if (ok) {
        memcpy(out, dec.pbData, dec.cbData);
        out[dec.cbData] = '\0';
    }
    SecureZeroMemory(dec.pbData, dec.cbData);
    LocalFree(dec.pbData);
    return ok;
}
