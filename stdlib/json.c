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

/* ---- depth-aware path/array accessors ----
 * Results are allocated from the thread-local arena (fr_arena_tls), not a
 * shared static buffer: callers routinely hold onto several results at once
 * (e.g. multiple json_get_path_str calls stored in different locals), and a
 * single shared buffer would silently overwrite earlier results. */

static const char *json_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Returns pointer just past the JSON value starting at p (p < end). */
static const char *json_value_end(const char *p, const char *end) {
    if (p >= end) return p;
    char c = *p;

    if (c == '"') {
        p++;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) { p += 2; continue; }
            if (*p == '"') { p++; break; }
            p++;
        }
        return p;
    }

    if (c == '{' || c == '[') {
        char open = c;
        char close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (p < end) {
            char ch = *p;
            if (ch == '"') {
                p++;
                while (p < end) {
                    if (*p == '\\' && p + 1 < end) { p += 2; continue; }
                    if (*p == '"') { p++; break; }
                    p++;
                }
                continue;
            }
            if (ch == open) {
                depth++;
            } else if (ch == close) {
                depth--;
                p++;
                if (depth == 0) break;
                continue;
            }
            p++;
        }
        return p;
    }

    while (p < end) {
        char ch = *p;
        if (ch == ',' || ch == '}' || ch == ']' ||
            ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
        p++;
    }
    return p;
}

/* Finds the direct child `key`'s value span inside the object [obj_start,obj_end)
   (obj_start must point at '{'). Returns 1 and sets val_start/val_end on success. */
static int json_find_child(const char *obj_start, const char *obj_end, const char *key,
                            const char **val_start, const char **val_end) {
    const char *p = obj_start;
    if (p >= obj_end || *p != '{') return 0;
    p++;

    size_t keylen = strlen(key);
    while (p < obj_end) {
        p = json_skip_ws(p, obj_end);
        if (p >= obj_end || *p == '}') break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') { p++; continue; }

        const char *kstart = p + 1;
        const char *kp = kstart;
        while (kp < obj_end && *kp != '"') {
            if (*kp == '\\' && kp + 1 < obj_end) kp += 2; else kp++;
        }
        size_t klen = (size_t)(kp - kstart);
        p = kp + 1;
        p = json_skip_ws(p, obj_end);
        if (p < obj_end && *p == ':') p++;
        p = json_skip_ws(p, obj_end);

        const char *vstart = p;
        const char *vend = json_value_end(p, obj_end);

        if (klen == keylen && strncmp(kstart, key, keylen) == 0) {
            *val_start = vstart;
            *val_end = vend;
            return 1;
        }
        p = vend;
    }
    return 0;
}

const char *fr_json_get_path_raw(const char *json, const char *path) {
    if (!json || !path) return "";

    const char *cur_start = json;
    const char *cur_end = json + strlen(json);
    const char *seg = path;
    const char *pend = path + strlen(path);

    while (seg < pend) {
        const char *dot = seg;
        while (dot < pend && *dot != '.') dot++;
        size_t seglen = (size_t)(dot - seg);

        char keybuf[256];
        if (seglen >= sizeof(keybuf)) seglen = sizeof(keybuf) - 1;
        memcpy(keybuf, seg, seglen);
        keybuf[seglen] = '\0';

        const char *ostart = json_skip_ws(cur_start, cur_end);
        if (ostart >= cur_end || *ostart != '{') return "";

        const char *val_start = NULL, *val_end = NULL;
        if (!json_find_child(ostart, cur_end, keybuf, &val_start, &val_end)) return "";

        cur_start = val_start;
        cur_end = val_end;
        seg = dot + 1;
    }

    size_t len = (size_t)(cur_end - cur_start);
    char *out = (char *)fr_arena_alloc(fr_arena_tls(), len + 1, 1);
    memcpy(out, cur_start, len);
    out[len] = '\0';
    return out;
}

const char *fr_json_get_path_string(const char *json, const char *path) {
    const char *raw = fr_json_get_path_raw(json, path);
    size_t n = strlen(raw);

    if (n >= 2 && raw[0] == '"' && raw[n - 1] == '"') {
        char *out = (char *)fr_arena_alloc(fr_arena_tls(), n, 1);
        size_t oi = 0;
        size_t i = 1;
        while (i < n - 1) {
            char c = raw[i];
            if (c == '\\' && i + 1 < n - 1) {
                char nc = raw[i + 1];
                switch (nc) {
                    case 'n': out[oi++] = '\n'; break;
                    case 't': out[oi++] = '\t'; break;
                    case 'r': out[oi++] = '\r'; break;
                    case '"': out[oi++] = '"'; break;
                    case '\\': out[oi++] = '\\'; break;
                    case '/': out[oi++] = '/'; break;
                    default: out[oi++] = nc; break;
                }
                i += 2;
                continue;
            }
            out[oi++] = c;
            i++;
        }
        out[oi] = '\0';
        return out;
    }

    return raw;
}

int64_t fr_json_get_path_int(const char *json, const char *path) {
    const char *raw = fr_json_get_path_raw(json, path);
    const char *p = raw;
    if (*p == '"') p++;
    return atoll(p);
}

int64_t fr_json_array_len(const char *json_array) {
    if (!json_array) return 0;
    const char *end = json_array + strlen(json_array);
    const char *p = json_skip_ws(json_array, end);
    if (p >= end || *p != '[') return 0;
    p++;

    p = json_skip_ws(p, end);
    if (p < end && *p == ']') return 0;

    int64_t count = 0;
    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *vend = json_value_end(p, end);
        count++;
        p = json_skip_ws(vend, end);
        if (p < end && *p == ',') { p++; continue; }
        break;
    }
    return count;
}

const char *fr_json_array_item(const char *json_array, int64_t index) {
    if (!json_array || index < 0) return "";

    const char *end = json_array + strlen(json_array);
    const char *p = json_skip_ws(json_array, end);
    if (p >= end || *p != '[') return "";
    p++;

    int64_t i = 0;
    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *vstart = p;
        const char *vend = json_value_end(p, end);

        if (i == index) {
            size_t vlen = (size_t)(vend - vstart);
            char *out = (char *)fr_arena_alloc(fr_arena_tls(), vlen + 1, 1);
            memcpy(out, vstart, vlen);
            out[vlen] = '\0';
            return out;
        }

        i++;
        p = json_skip_ws(vend, end);
        if (p < end && *p == ',') { p++; continue; }
        break;
    }
    return "";
}
