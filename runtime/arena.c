#include "forge/arena.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Arena implementation: a linked list ("chunk list") of fixed blocks.
 *
 * Growth never reallocates (and thus never moves) a block that may already
 * have live pointers into it -- when the current block can't satisfy an
 * allocation, a brand new block is spliced into the chain and becomes the
 * new "current" block. Previously returned pointers stay valid for the
 * lifetime of the arena (until fr_arena_reset/fr_arena_destroy), because the
 * block that backs them is never freed or moved while the arena is alive.
 *
 * fr_arena_reset rewinds to the first block but keeps every block allocated,
 * so a reset-and-refill arena (per-coroutine scratch, per-request bodies)
 * settles into reusing the same chain with no allocator traffic at all.
 *
 * An arena is single-owner: it is not safe to allocate from one arena from
 * two threads at once. The coroutine/thread-local plumbing at the bottom of
 * this file is what keeps that true in practice.
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
/* Written once by the runtime at scheduler creation, read by every worker
 * thread on every fr_arena_tls() call -- atomic so that publication is a
 * defined cross-thread transfer rather than a data race. */
static _Atomic(fr_arena_coro_provider_fn) g_coro_provider = NULL;

void fr_arena_set_coro_provider(fr_arena_coro_provider_fn fn) {
    atomic_store_explicit(&g_coro_provider, fn, memory_order_release);
}

/* The coroutine arena for the calling context, or NULL when there is no
 * coroutine running (or no provider registered). */
static fr_arena_t *current_coro_arena(void) {
    fr_arena_coro_provider_fn provider =
        atomic_load_explicit(&g_coro_provider, memory_order_acquire);
    return provider ? provider() : NULL;
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

/* Overflow-checked bump allocation inside one block. Aligns the returned
 * *address* rather than the offset within the block: `data` sits behind a
 * header, so an aligned offset is not an aligned pointer. Returns NULL if
 * the block cannot satisfy the request. */
static void *block_bump(fr_arena_block_t *b, size_t size, size_t align) {
    uintptr_t base = (uintptr_t)b->data;
    uintptr_t cursor = base + b->pos;
    uintptr_t aligned = (cursor + (align - 1)) & ~(uintptr_t)(align - 1);
    if (aligned < cursor) return NULL; /* alignment round-up wrapped */

    size_t off = (size_t)(aligned - base);
    size_t end;
    if (off > b->cap) return NULL;
    if (__builtin_add_overflow(off, size, &end) || end > b->cap) return NULL;

    b->pos = end;
    return b->data + off;
}

void *fr_arena_alloc(fr_arena_t *a, size_t size, size_t align) {
    if (!a || size == 0) return NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    /* Alignment must be a power of two for the round-up below to be a
     * round-up at all; round a bogus value up to one rather than silently
     * handing back a misaligned pointer. */
    if (align & (align - 1)) {
        size_t pow2 = sizeof(void *);
        while (pow2 < align) pow2 <<= 1;
        align = pow2;
    }

    void *p = block_bump(a->current, size, align);
    if (p) return p;

    /* The current block is full. Before allocating, look for an already-owned
     * successor big enough: after fr_arena_reset the whole chain is still
     * there with pos == 0, so this is the steady-state path for an arena that
     * is reset and refilled repeatedly. */
    for (fr_arena_block_t *b = a->current->next; b; b = b->next) {
        p = block_bump(b, size, align);
        if (p) {
            a->current = b;
            return p;
        }
    }

    /* Grow with a brand new block -- never by reallocating (and thus moving)
     * a block that may already have live pointers into it. */
    size_t block_cap = a->default_cap;
    size_t need;
    if (__builtin_add_overflow(size, align - 1, &need)) return NULL;
    if (need > block_cap) block_cap = need; /* oversized request gets its own block */

    fr_arena_block_t *nb = arena_block_create(block_cap);
    if (!nb) return NULL;

    /* Splice in after the current block rather than overwriting its `next`:
     * appending blindly used to orphan the entire remainder of the chain
     * (everything allocated before the last reset), leaking it -- those
     * blocks were no longer reachable from a->first for fr_arena_destroy. */
    nb->next = a->current->next;
    a->current->next = nb;
    a->current = nb;

    return block_bump(nb, size, align);
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
    fr_arena_t *coro_arena = current_coro_arena();
    if (coro_arena) return coro_arena;
    if (!tls_arena) tls_arena = fr_arena_create(4 * 1024 * 1024);
    return tls_arena;
}

void fr_arena_tls_reset(void) {
    fr_arena_t *arena = current_coro_arena();
    if (!arena) arena = tls_arena;
    fr_arena_reset(arena);
}
