#include "json.h"
#include <string.h>

/* ---- Tokeniser ---- */

typedef struct {
    const char *js;
    int len;
    int pos;
    JsonToken *tokens;
    int max;
    int count;
} Parser;

static int NewToken(Parser *p, JsonType type, int start, int parent)
{
    if (p->count >= p->max)
        return JSON_ERROR_NOMEM;
    JsonToken *t = &p->tokens[p->count];
    t->type = type;
    t->start = start;
    t->end = -1;
    t->size = 0;
    t->parent = parent;
    return p->count++;
}

static void SkipSpace(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        p->pos++;
    }
}

static int IsHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* p->pos is on the opening quote. */
static int ParseString(Parser *p, int parent)
{
    int start = ++p->pos;
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == '"') {
            int i = NewToken(p, JSON_STRING, start, parent);
            if (i < 0) return i;
            p->tokens[i].end = p->pos++;
            return i;
        }
        if ((unsigned char)c < 0x20)
            return JSON_ERROR_INVALID;
        if (c == '\\') {
            if (++p->pos >= p->len) return JSON_ERROR_INVALID;
            switch (p->js[p->pos]) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                break;
            case 'u':
                for (int k = 1; k <= 4; k++)
                    if (p->pos + k >= p->len || !IsHex(p->js[p->pos + k]))
                        return JSON_ERROR_INVALID;
                p->pos += 4;
                break;
            default:
                return JSON_ERROR_INVALID;
            }
        }
        p->pos++;
    }
    return JSON_ERROR_INVALID;   /* unterminated */
}

static int ParsePrimitive(Parser *p, int parent)
{
    int start = p->pos;
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        if ((unsigned char)c < 0x20 || c == '"' || c == ':' || c == '[' || c == '{')
            return JSON_ERROR_INVALID;
        p->pos++;
    }
    if (p->pos == start)
        return JSON_ERROR_INVALID;
    const char *s = p->js + start;
    int n = p->pos - start;
    char c0 = s[0];
    int ok = (n == 4 && memcmp(s, "true", 4) == 0) || (n == 5 && memcmp(s, "false", 5) == 0) ||
             (n == 4 && memcmp(s, "null", 4) == 0) || c0 == '-' || (c0 >= '0' && c0 <= '9');
    if (!ok)
        return JSON_ERROR_INVALID;
    int i = NewToken(p, JSON_PRIMITIVE, start, parent);
    if (i < 0) return i;
    p->tokens[i].end = p->pos;
    return i;
}

static int ParseValue(Parser *p, int parent, int depth);

/* p->pos is on '{' or '['. */
static int ParseContainer(Parser *p, int parent, int depth)
{
    int isObject = (p->js[p->pos] == '{');
    char close = isObject ? '}' : ']';
    int i = NewToken(p, isObject ? JSON_OBJECT : JSON_ARRAY, p->pos, parent);
    if (i < 0) return i;
    p->pos++;
    SkipSpace(p);
    if (p->pos < p->len && p->js[p->pos] == close) {
        p->tokens[i].end = ++p->pos;
        return i;
    }
    for (;;) {
        SkipSpace(p);
        if (isObject) {
            if (p->pos >= p->len || p->js[p->pos] != '"')
                return JSON_ERROR_INVALID;
            int key = ParseString(p, i);
            if (key < 0) return key;
            SkipSpace(p);
            if (p->pos >= p->len || p->js[p->pos] != ':')
                return JSON_ERROR_INVALID;
            p->pos++;
            /* The value hangs off the key token, so a member reads key, value. */
            int v = ParseValue(p, key, depth + 1);
            if (v < 0) return v;
            p->tokens[key].size = 1;
        } else {
            int v = ParseValue(p, i, depth + 1);
            if (v < 0) return v;
        }
        p->tokens[i].size++;
        SkipSpace(p);
        if (p->pos >= p->len)
            return JSON_ERROR_INVALID;
        char c = p->js[p->pos];
        if (c == ',') {
            p->pos++;
            continue;
        }
        if (c == close) {
            p->tokens[i].end = ++p->pos;
            return i;
        }
        return JSON_ERROR_INVALID;
    }
}

static int ParseValue(Parser *p, int parent, int depth)
{
    if (depth > 64)
        return JSON_ERROR_INVALID;   /* guards the recursion against hostile input */
    SkipSpace(p);
    if (p->pos >= p->len)
        return JSON_ERROR_INVALID;
    char c = p->js[p->pos];
    if (c == '{' || c == '[')
        return ParseContainer(p, parent, depth);
    if (c == '"')
        return ParseString(p, parent);
    return ParsePrimitive(p, parent);
}

int Json_Parse(const char *js, int len, JsonToken *tokens, int maxTokens)
{
    Parser p = { js, len, 0, tokens, maxTokens, 0 };
    int root = ParseValue(&p, -1, 0);
    if (root < 0)
        return root;
    SkipSpace(&p);
    if (p.pos != p.len)
        return JSON_ERROR_INVALID;   /* trailing garbage */
    return p.count;
}

/* ---- Navigation ---- */

int Json_Skip(const JsonToken *tokens, int count, int i)
{
    /* Tokens are stored in document order, so the subtree of i is the run of
       tokens that start before i ends. */
    int end = tokens[i].end;
    int j = i + 1;
    while (j < count && tokens[j].start < end)
        j++;
    return j;
}

int Json_Get(const char *js, const JsonToken *tokens, int count, int obj, const char *key)
{
    if (obj < 0 || obj >= count || tokens[obj].type != JSON_OBJECT)
        return -1;
    int j = obj + 1;
    for (int m = 0; m < tokens[obj].size && j < count; m++) {
        int value = j + 1;
        if (Json_IsString(js, &tokens[j], key))
            return value < count ? value : -1;
        j = Json_Skip(tokens, count, value);
    }
    return -1;
}

int Json_At(const JsonToken *tokens, int count, int arr, int n)
{
    if (arr < 0 || arr >= count || tokens[arr].type != JSON_ARRAY || n < 0 || n >= tokens[arr].size)
        return -1;
    int j = arr + 1;
    for (int k = 0; k < n; k++)
        j = Json_Skip(tokens, count, j);
    return j < count ? j : -1;
}

int Json_IsString(const char *js, const JsonToken *t, const char *s)
{
    int n = (int)strlen(s);
    return t->type == JSON_STRING && t->end - t->start == n &&
           memcmp(js + t->start, s, (size_t)n) == 0;
}

int Json_IsTrue(const char *js, const JsonToken *t)
{
    return t->type == JSON_PRIMITIVE && t->end - t->start == 4 &&
           memcmp(js + t->start, "true", 4) == 0;
}

/* ---- Strings ---- */

static int HexValue(const char *s)
{
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else v |= c - 'A' + 10;
    }
    return v;
}

static int PutUtf8(char *out, int len, int cap, unsigned cp)
{
    char buf[4];
    int n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (len + n >= cap)
        return -1;
    memcpy(out + len, buf, (size_t)n);
    return len + n;
}

int Json_GetString(const char *js, const JsonToken *t, char *out, int cap)
{
    if (cap <= 0 || t->end < t->start)
        return -1;   /* also a token left half-built by a failed parse */
    int len = 0;
    if (t->type != JSON_STRING) {
        int n = t->end - t->start;
        if (n >= cap) return -1;
        memcpy(out, js + t->start, (size_t)n);
        out[n] = '\0';
        return n;
    }
    for (int i = t->start; i < t->end; i++) {
        char c = js[i];
        if (c != '\\') {
            if (len + 1 >= cap) return -1;
            out[len++] = c;
            continue;
        }
        char e = js[++i];
        unsigned cp;
        switch (e) {
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            cp = (unsigned)HexValue(js + i + 1);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                /* A high surrogate must be followed by \u and a low one;
                   that escape takes js[i + 1] to js[i + 6]. */
                if (i + 6 >= t->end || js[i + 1] != '\\' || js[i + 2] != 'u')
                    return -1;
                unsigned lo = (unsigned)HexValue(js + i + 3);
                if (lo < 0xDC00 || lo > 0xDFFF)
                    return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1;   /* a lone low surrogate */
            }
            break;
        default:  cp = (unsigned char)e; break;   /* \" \\ \/ */
        }
        if (cp == 0)
            return -1;   /* \u0000 would end the C string early and hide the rest */
        len = PutUtf8(out, len, cap, cp);
        if (len < 0) return -1;
    }
    out[len] = '\0';
    return len;
}

int Json_Quote(const char *s, char *out, int cap)
{
    static const char hex[] = "0123456789abcdef";
    int len = 0;
    if (cap < 3) return -1;
    out[len++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc = 0;
        switch (c) {
        case '"':  esc = '"';  break;
        case '\\': esc = '\\'; break;
        case '\n': esc = 'n';  break;
        case '\r': esc = 'r';  break;
        case '\t': esc = 't';  break;
        case '\b': esc = 'b';  break;
        case '\f': esc = 'f';  break;
        }
        if (esc) {
            if (len + 2 >= cap) return -1;
            out[len++] = '\\';
            out[len++] = esc;
        } else if (c < 0x20) {
            if (len + 6 >= cap) return -1;
            out[len++] = '\\'; out[len++] = 'u'; out[len++] = '0'; out[len++] = '0';
            out[len++] = hex[c >> 4];
            out[len++] = hex[c & 15];
        } else {
            if (len + 1 >= cap) return -1;
            out[len++] = (char)c;
        }
    }
    if (len + 2 > cap) return -1;
    out[len++] = '"';
    out[len] = '\0';
    return len;
}
