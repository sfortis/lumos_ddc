#ifndef JSON_H
#define JSON_H

/* A small JSON reader for the Home Assistant replies, without allocations.
 *
 * Json_Parse splits a document into a flat array of tokens, the way jsmn
 * does: every object, array, string and primitive (number, true, false,
 * null) is one token that records where it starts and ends in the text,
 * how many direct children it has, and its parent. A member of an object
 * is two tokens, the key and the value. The helpers below walk that array.
 *
 * Strings come back in UTF-8 with their escapes decoded; turning them into
 * UTF-16 is left to the Win32 side. Win32-free, unit-tested in test_json.c. */

typedef enum {
    JSON_UNDEFINED = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_PRIMITIVE
} JsonType;

typedef struct {
    JsonType type;
    int start;    /* offset of the first character (inside the quotes for a string) */
    int end;      /* offset one past the last character */
    int size;     /* direct children: members (key/value pairs) or elements */
    int parent;   /* index of the enclosing token, -1 for the root */
} JsonToken;

#define JSON_ERROR_NOMEM   -1   /* more tokens than maxTokens */
#define JSON_ERROR_INVALID -2   /* not valid JSON */

/* Tokenise len bytes of js. Returns the number of tokens, or a JSON_ERROR_*. */
int Json_Parse(const char *js, int len, JsonToken *tokens, int maxTokens);

/* The index of the token that follows token i and all of its children. */
int Json_Skip(const JsonToken *tokens, int count, int i);

/* The value token of member key in object token obj, or -1. */
int Json_Get(const char *js, const JsonToken *tokens, int count, int obj, const char *key);

/* The value token of element n in array token arr, or -1. */
int Json_At(const JsonToken *tokens, int count, int arr, int n);

/* Nonzero when token i is a string equal to s. */
int Json_IsString(const char *js, const JsonToken *t, const char *s);

/* Nonzero when token i is the primitive true. */
int Json_IsTrue(const char *js, const JsonToken *t);

/* Decode string token t (escapes and \u sequences, surrogate pairs included)
   into out as UTF-8 and terminate it. A primitive is copied as its text.
   Returns the length, or -1 when out is too small or the escape is invalid. */
int Json_GetString(const char *js, const JsonToken *t, char *out, int cap);

/* Write s as a JSON string literal, quotes included, into out. Returns the
   length, or -1 when out is too small. */
int Json_Quote(const char *s, char *out, int cap);

#endif /* JSON_H */
