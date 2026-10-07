#include "hassurl.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static int Norm(const wchar_t *in, const wchar_t *expect)
{
    wchar_t out[256];
    int ok = HassUrl_Normalize(in, out, 256);
    return expect ? ok && wcscmp(out, expect) == 0 : !ok;
}

int main(void)
{
    /* Normalize */
    CHECK(Norm(L"http://192.168.1.10:8123", L"http://192.168.1.10:8123"), "a full URL stays as it is");
    CHECK(Norm(L"https://ha.example.com", L"https://ha.example.com"), "https stays");
    CHECK(Norm(L"192.168.1.10:8123", L"http://192.168.1.10:8123"), "no scheme: http is added");
    CHECK(Norm(L"homeassistant.local:8123", L"http://homeassistant.local:8123"), "a name and a port is not a scheme");
    CHECK(Norm(L"ha.local:8123/path", L"http://ha.local:8123/path"), "a path after the port");
    CHECK(Norm(L"  http://192.168.1.10:8123 \t\r\n", L"http://192.168.1.10:8123"), "white space trimmed");
    CHECK(Norm(L" 10.0.0.5 ", L"http://10.0.0.5"), "trimmed and http added");
    CHECK(Norm(L"HTTP://X", L"HTTP://X"), "the case of the scheme is kept");
    CHECK(Norm(L"", NULL) && Norm(L"   ", NULL), "empty text is refused");
    wchar_t small[10];
    CHECK(!HassUrl_Normalize(L"192.168.1.10", small, 10), "a result that does not fit is refused");
    wchar_t exact[11];   /* "http://a:8" is 10 characters plus the terminator */
    CHECK(HassUrl_Normalize(L"a:8", exact, 11) && wcscmp(exact, L"http://a:8") == 0, "exactly fits");
    CHECK(!HassUrl_Normalize(L"a:8", exact, 10), "one short is refused");

    /* IsHttp */
    CHECK(HassUrl_IsHttp(L"http://x") && HassUrl_IsHttp(L"HTTP://x"), "http in any case");
    CHECK(!HassUrl_IsHttp(L"https://x") && !HassUrl_IsHttp(L"httpx://x"), "not http");

    /* IsLocal */
    const wchar_t *local[] = {
        L"http://192.168.1.10:8123", L"http://10.0.0.5", L"http://172.16.0.1:8123", L"http://172.31.255.255",
        L"http://127.0.0.1:8123", L"http://169.254.10.1", L"http://homeassistant.local:8123",
        L"http://HA.LOCAL./", L"http://ha.lan", L"http://ha.internal:8123", L"http://ha.home.arpa",
        L"http://homeassistant:8123", L"http://localhost:8123", L"http://[fd00::10]:8123",
        L"http://[fe80::1%25eth0]:8123", L"http://[::1]:8123", L"http://user:pw@192.168.1.10:8123/x?y#z",
    };
    const wchar_t *remote[] = {
        L"http://ha.example.com", L"http://8.8.8.8", L"http://172.32.0.1", L"http://172.15.0.1",
        L"http://192.169.1.1", L"http://[2001:db8::1]:8123", L"http://ha.local.example.com",
        L"http://256.1.1.1.example", L"http://1.2.3", L"http://[::ffff:192.168.1.1]",
        L"not a url", L"http://",
        L"http://134744072", L"http://0x08080808", L"http://010.0.0.1", L"http://192.168.01.1",
        L"http://evil.com\\@10.0.0.1/", L"http://[fe80::1]evil.com", L"http://.", L"http://evil%2elocal",
        L"http://[1fc00::1]", L"http://[fffffffff::1]", L"http://8ball",
    };
    char msg[160];
    for (size_t i = 0; i < sizeof local / sizeof local[0]; i++) {
        snprintf(msg, sizeof msg, "local: %ls", local[i]);
        CHECK(HassUrl_IsLocal(local[i]), msg);
    }
    for (size_t i = 0; i < sizeof remote / sizeof remote[0]; i++) {
        snprintf(msg, sizeof msg, "not local: %ls", remote[i]);
        CHECK(!HassUrl_IsLocal(remote[i]), msg);
    }

    if (failures == 0)
        printf("All hassurl tests passed.\n");
    return failures ? 1 : 0;
}
