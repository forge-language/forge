#include "forge/arena.h"
#include <stdlib.h>
#include <string.h>

/*
 * Arena implementation: a linked list ("chunk list") of fixed blocks.
 *
 * Growth never reallocates (and thus never moves) a block that may already
 * have live pointers into it -- when the current block can't satisfy an
 * allocation, a brand new block is appended and becomes the new "current"
 * block. Previously returned pointers stay valid for the lifetime of the
 * arena (until fr_arena_reset/fr_arena_destroy), because the block that
 * backs them is never freed or moved while the arena is alive.
 */

#define FR_ARENA_MIN_BLOCK ((size_t)4096)

typedef struct fr_arena_block {
    struct fr_arena_block *next;
    size_t cap;
    size_t pos;
    char data[];
} fr_arena_block_t;

struct fr_arena {
    fr_arena_block_t *first;
    fr_arena_block_t *current;
    size_t default_cap;
};

static _Thread_local fr_arena_t *tls_arena = NULL;
static fr_arena_coro_provider_fn g_coro_provider = NULL;

void fr_arena_set_coro_provider(fr_arena_coro_provider_fn fn) {
    g_coro_provider = fn;
}

static fr_arena_block_t *arena_block_create(size_t cap) {
    size_t total;
    if (__builtin_add_overflow(sizeof(fr_arena_block_t), cap, &total)) return NULL;
    fr_arena_block_t *b = (fr_arena_block_t *)malloc(total);
    if (!b) return NULL;
    b->next = NULL;
    b->cap = cap;
    b->pos = 0;
    return b;
}

fr_arena_t *fr_arena_create(size_t initial_cap) {
    fr_arena_t *a = (fr_arena_t *)calloc(1, sizeof(fr_arena_t));
    if (!a) return NULL;
    if (initial_cap < FR_ARENA_MIN_BLOCK) initial_cap = FR_ARENA_MIN_BLOCK;
    fr_arena_block_t *b = arena_block_create(initial_cap);
    if (!b) {
        free(a);
        return NULL;
    }
    a->first = b;
    a->current = b;
    a->default_cap = initial_cap;
    return a;
}

void fr_arena_destroy(fr_arena_t *a) {
    if (!a) return;
    fr_arena_block_t *b = a->first;
    while (b) {
        fr_arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

void *fr_arena_alloc(fr_arena_t *a, size_t size, size_t align) {
    if (!a || size == 0) return NULL;
    if (align && (align & (align - 1))) return NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    size_t required;
    if (__builtin_add_overflow(size, align - 1, &required)) return NULL;
    fr_arena_block_t *b = a->current;
    for (;;) {
        /* Align the actual address, not just the offset from data[]. */
        uintptr_t address = (uintptr_t)(b->data + b->pos);
        size_t padding = (size_t)(-address & (align - 1));
        size_t off, end;
        if (!__builtin_add_overflow(b->pos, padding, &off) &&
            !__builtin_add_overflow(off, size, &end) && end <= b->cap) {
            b->pos = end;
            a->current = b;
            return b->data + off;
        }
        /* Reset retains blocks. Reuse them without losing the tail chain. */
        if (b->next) {
            b = b->next;
            continue;
        }
        size_t block_cap = a->default_cap;
        if (required > block_cap) block_cap = required;
        fr_arena_block_t *nb = arena_block_create(block_cap);
        if (!nb) return NULL;
        b->next = nb;
        b = nb;
    }
}

char *fr_arena_strdup(fr_arena_t *a, const char *s) {
    if (!a || !s) return NULL;
    size_t n = strlen(s) + 1;
    char *out = (char *)fr_arena_alloc(a, n, 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    return out;
}

void fr_arena_reset(fr_arena_t *a) {
    if (!a) return;
    for (fr_arena_block_t *b = a->first; b; b = b->next) {
        b->pos = 0;
    }
    a->current = a->first;
}

fr_arena_t *fr_arena_tls(void) {
    if (g_coro_provider) {
        fr_arena_t *coro_arena = g_coro_provider();
        if (coro_arena) return coro_arena;
    }
    if (!tls_arena) tls_arena = fr_arena_create(4 * 1024 * 1024);
    return tls_arena;
}

void fr_arena_tls_reset(void) {
    if (g_coro_provider) {
        fr_arena_t *coro_arena = g_coro_provider();
        if (coro_arena) {
            fr_arena_reset(coro_arena);
            return;
        }
    }
    if (tls_arena) fr_arena_reset(tls_arena);
}
