#ifndef FORGE_COMMON_H
#define FORGE_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include "source.h"

#define FORGE_VERSION "0.3.0"

typedef struct ForgeStr {
    char *data;
    size_t len;
} ForgeStr;

static inline ForgeStr forge_str(const char *s) {
    ForgeStr r = { (char *)s, s ? strlen(s) : 0 };
    return r;
}

static inline bool forge_str_eq(ForgeStr a, ForgeStr b) {
    return a.len == b.len && strncmp(a.data, b.data, a.len) == 0;
}

static inline char *forge_strdup(ForgeStr s) {
    char *out = (char *)malloc(s.len + 1);
    if (!out) return NULL;
    memcpy(out, s.data, s.len);
    out[s.len] = '\0';
    return out;
}

[[noreturn]] static inline void forge_die(const char *msg) {
    fprintf(stderr, "forge: fatal: %s\n", msg);
    exit(1);
}

/* Keep the old allocation alive until realloc succeeds, and bound both
 * geometric capacity growth and its conversion to a byte count. */
static inline void *forge_array_reserve(void *items, size_t count, size_t *capacity,
                                        size_t item_size, size_t initial_capacity) {
    if (count < *capacity) return items;
    size_t maximum = SIZE_MAX / item_size;
    if (count >= maximum) forge_die("too many array elements");
    size_t next = *capacity ? *capacity : initial_capacity;
    if (next <= count) next = next <= maximum / 2 ? next * 2 : maximum;
    if (next > maximum) next = maximum;
    void *grown = realloc(items, next * item_size);
    if (!grown) forge_die("out of memory growing array");
    *capacity = next;
    return grown;
}

static inline void *forge_calloc_array(size_t count, size_t item_size) {
    if (count > SIZE_MAX / item_size) forge_die("too many array elements");
    void *items = calloc(count, item_size);
    if (!items) forge_die("out of memory");
    return items;
}

#endif
