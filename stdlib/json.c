#include "forge/json.h"
#include "forge/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_key_value(const char *json, const char *key, char quote) {
    if (!json || !key) return NULL;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (quote == '\0') {
        /* No specific quote required: value is a bare token (e.g. a
         * number). Skip an optional leading quote so callers that
         * accept quoted numeric strings still work. */
        if (*p == '"') p++;
        return p;
    }
    if (*p != quote) return NULL;
    return p + 1;
}

const char *fr_json_get_string(const char *json, const char *key) {
    const char *start = find_key_value(json, key, '"');
    if (!start) return "";
    const char *end = strchr(start, '"');
    if (!end) return "";
    size_t len = (size_t)(end - start);

    /* Allocate from the thread-local arena instead of a shared `static`
     * buffer: a static buffer is a data race across threads and also gets
     * clobbered if two extracted values are held at the same time (the
     * second call overwrites the first). The arena gives every call its
     * own independent memory, with no artificial length cap. */
    fr_arena_t *arena = fr_arena_tls();
    char *out = (char *)fr_arena_alloc(arena, len + 1, 1);
    if (!out) return "";
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

int64_t fr_json_get_int(const char *json, const char *key) {
    const char *start = find_key_value(json, key, '\0');
    if (!start) return 0;
    return atoll(start);
}

/* Writes the JSON-escaped form of `s` into out[0..out_cap), NUL-terminated.
 * Escapes '"', '\\', the common control-character shorthands, and any other
 * byte < 0x20 as \u00XX, so a caller-supplied string can never break out of
 * the surrounding JSON string literal (JSON injection). */
static void json_escape(const char *s, char *out, size_t out_cap) {
    size_t o = 0;
    if (!s || out_cap == 0) {
        if (out_cap) out[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        char esc[8];
        const char *rep = NULL;
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            case '\b': rep = "\\b"; break;
            case '\f': rep = "\\f"; break;
            default:
                if (c < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    rep = esc;
                }
                break;
        }
        if (rep) {
            size_t rlen = strlen(rep);
            if (o + rlen >= out_cap) break;
            memcpy(out + o, rep, rlen);
            o += rlen;
        } else {
            if (o + 1 >= out_cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

char *fr_json_stringify_str(const char *key, const char *value) {
    /* Worst case every byte expands to \u00XX (6 chars); +1 for the NUL. */
    size_t key_cap = (key ? strlen(key) : 0) * 6 + 1;
    size_t val_cap = (value ? strlen(value) : 0) * 6 + 1;
    char *ekey = (char *)malloc(key_cap);
    char *evalue = (char *)malloc(val_cap);
    if (!ekey || !evalue) {
        free(ekey);
        free(evalue);
        return NULL;
    }
    json_escape(key, ekey, key_cap);
    json_escape(value, evalue, val_cap);

    size_t cap = strlen(ekey) + strlen(evalue) + 16;
    char *out = (char *)malloc(cap);
    if (out) snprintf(out, cap, "{\"%s\":\"%s\"}", ekey, evalue);
    free(ekey);
    free(evalue);
    return out;
}

char *fr_json_stringify_int(const char *key, int64_t value) {
    char *out = (char *)malloc(256);
    if (!out) return NULL;
    snprintf(out, 256, "{\"%s\":%lld}", key ? key : "", (long long)value);
    return out;
}
