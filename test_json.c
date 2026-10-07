#include "json.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static JsonToken tok[256];

static int Parse(const char *js)
{
    return Json_Parse(js, (int)strlen(js), tok, 256);
}

static int StringOf(const char *js, int n, int obj, const char *key, char *out, int cap)
{
    int v = Json_Get(js, tok, n, obj, key);
    return v < 0 ? -1 : Json_GetString(js, &tok[v], out, cap);
}

int main(void)
{
    char buf[256];
    int n;

    /* A Home Assistant state reply */
    const char *state =
        "{\"entity_id\":\"sensor.fp2_light\",\"state\":\"5\","
        "\"attributes\":{\"unit_of_measurement\":\"lx\",\"device_class\":\"illuminance\","
        "\"friendly_name\":\"Presence Sensor FP2 Light Level\"},"
        "\"last_changed\":\"2026-10-07T06:00:00+00:00\",\"context\":{\"id\":\"x\",\"parent_id\":null}}";
    n = Parse(state);
    CHECK(n > 0, "state parses");
    CHECK(StringOf(state, n, 0, "state", buf, sizeof buf) == 1 && strcmp(buf, "5") == 0, "state value");
    int attrs = Json_Get(state, tok, n, 0, "attributes");
    CHECK(attrs > 0 && tok[attrs].type == JSON_OBJECT, "attributes object");
    CHECK(StringOf(state, n, attrs, "device_class", buf, sizeof buf) > 0 &&
          strcmp(buf, "illuminance") == 0, "nested key");
    CHECK(StringOf(state, n, 0, "last_changed", buf, sizeof buf) > 0, "key after a nested object");
    CHECK(Json_Get(state, tok, n, 0, "missing") == -1, "missing key");
    int ctx = Json_Get(state, tok, n, 0, "context");
    CHECK(StringOf(state, n, ctx, "parent_id", buf, sizeof buf) == 4 && strcmp(buf, "null") == 0,
          "primitive copied as text");

    /* WebSocket messages */
    const char *ok = "{\"type\":\"result\",\"id\":1,\"success\":true,\"result\":null}";
    n = Parse(ok);
    CHECK(n > 0 && Json_IsString(ok, &tok[Json_Get(ok, tok, n, 0, "type")], "result"), "type result");
    CHECK(Json_IsTrue(ok, &tok[Json_Get(ok, tok, n, 0, "success")]), "success true");
    const char *ev = "{\"id\":1,\"type\":\"event\",\"event\":{\"result\":"
                     "\"sensor.a\\tLiving Room \\u00b7 FP2\\tLiving Room\\t5\\n\","
                     "\"listeners\":{\"all\":false,\"entities\":[\"sensor.a\"],\"domains\":[]}}}";
    n = Parse(ev);
    CHECK(n > 0, "event parses");
    int evo = Json_Get(ev, tok, n, 0, "event");
    CHECK(StringOf(ev, n, evo, "result", buf, sizeof buf) > 0 &&
          strcmp(buf, "sensor.a\tLiving Room \xc2\xb7 FP2\tLiving Room\t5\n") == 0, "escapes decoded to UTF-8");

    /* Arrays */
    const char *arr = "[1, \"two\", {\"x\": [3, 4]}, [], {}, false]";
    n = Parse(arr);
    CHECK(n > 0 && tok[0].type == JSON_ARRAY && tok[0].size == 6, "array size");
    CHECK(Json_GetString(arr, &tok[Json_At(tok, n, 0, 1)], buf, sizeof buf) == 3 && strcmp(buf, "two") == 0,
          "array element");
    int third = Json_At(tok, n, 0, 2);
    CHECK(tok[third].type == JSON_OBJECT, "object element");
    CHECK(tok[Json_At(tok, n, 0, 3)].type == JSON_ARRAY && tok[Json_At(tok, n, 0, 3)].size == 0, "empty array");
    CHECK(Json_At(tok, n, 0, 6) == -1, "index past the end");
    CHECK(Json_GetString(arr, &tok[Json_At(tok, n, 0, 5)], buf, sizeof buf) == 5, "last element after empties");

    /* Surrogate pairs and invalid escapes */
    const char *emoji = "\"\\ud83d\\ude00!\"";
    n = Parse(emoji);
    CHECK(n == 1 && Json_GetString(emoji, &tok[0], buf, sizeof buf) == 5 &&
          memcmp(buf, "\xf0\x9f\x98\x80!", 5) == 0, "surrogate pair");
    const char *lone = "\"\\ud83d\"";
    n = Parse(lone);
    CHECK(n == 1 && Json_GetString(lone, &tok[0], buf, sizeof buf) == -1, "lone high surrogate rejected");
    const char *nul = "\"auth_ok\\u0000x\"";
    n = Parse(nul);
    CHECK(n == 1 && Json_GetString(nul, &tok[0], buf, sizeof buf) == -1, "embedded NUL rejected");
    const char *cut = "\"\\ud83d\\u\"";
    CHECK(Parse(cut) < 0, "truncated second escape rejected by the parser");

    /* Errors */
    CHECK(Parse("{\"a\":1,}") == JSON_ERROR_INVALID, "trailing comma");
    CHECK(Parse("{\"a\" 1}") == JSON_ERROR_INVALID, "missing colon");
    CHECK(Parse("[1 2]") == JSON_ERROR_INVALID, "missing comma");
    CHECK(Parse("\"open") == JSON_ERROR_INVALID, "unterminated string");
    CHECK(Parse("{\"a\":tru}") == JSON_ERROR_INVALID, "bad literal");
    CHECK(Parse("{} x") == JSON_ERROR_INVALID, "trailing garbage");
    CHECK(Parse("\"a\x01\"") == JSON_ERROR_INVALID, "control character in a string");
    CHECK(Parse("") == JSON_ERROR_INVALID, "empty input");
    char deep[200];
    memset(deep, '[', 100); memset(deep + 100, ']', 100);
    CHECK(Json_Parse(deep, 200, tok, 256) == JSON_ERROR_INVALID, "nesting deeper than 64");
    JsonToken few[2];
    CHECK(Json_Parse("[1,2,3]", 7, few, 2) == JSON_ERROR_NOMEM, "too many tokens");
    n = Parse("\"abcdef\"");
    CHECK(Json_GetString("\"abcdef\"", &tok[0], buf, 4) == -1, "string longer than the buffer");

    /* Quote round trip */
    const char *raw = "say \"hi\"\\\n\t\x01 \xce\xb1";
    char quoted[128];
    int q = Json_Quote(raw, quoted, sizeof quoted);
    CHECK(q > 0 && strcmp(quoted, "\"say \\\"hi\\\"\\\\\\n\\t\\u0001 \xce\xb1\"") == 0, "quote escapes");
    n = Parse(quoted);
    CHECK(n == 1 && Json_GetString(quoted, &tok[0], buf, sizeof buf) > 0 && strcmp(buf, raw) == 0, "quote round trip");
    CHECK(Json_Quote("abc", quoted, 5) == -1, "quote into a small buffer");

    if (failures == 0)
        printf("All json tests passed.\n");
    return failures ? 1 : 0;
}
