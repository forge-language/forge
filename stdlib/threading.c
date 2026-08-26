#include "forge/threading.h"
#include "forge/platform.h"
#include "forge/thread.h"
#include "forge_runtime.h"
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#define THREAD_MAX_HANDLES 256
#define THREAD_MAX_MUTEXES 64

typedef struct {
    fr_thread_t *thread;
    int in_use;
} thread_slot_t;

typedef struct {
    fr_mutex_t *mutex;
    int refcount;        /* number of lock() calls without a matching unlock() */
    int pending_destroy; /* destroy() was requested while refcount > 0 */
} mutex_slot_t;

static thread_slot_t g_threads[THREAD_MAX_HANDLES];
static mutex_slot_t g_mutexes[THREAD_MAX_MUTEXES];
static fr_mutex_t *g_registry_lock;
static int64_t g_next_thread_id;
static fr_mutex_t *g_id_lock;

#if defined(_WIN32)
static __declspec(thread) int64_t tls_worker_id = -1;
#else
static __thread int64_t tls_worker_id = -1;
#endif

/* Thread-safe one-time registry initialization. A plain "if (!g_registry_lock)
 * check-then-init" is racy: two threads can both observe an uninitialized
 * registry and both allocate/re-memset it concurrently, corrupting state or
 * leaking the loser's allocation. Use the platform's proper once-init
 * primitive so initialization happens exactly once no matter how many
 * threads race into registry_init() simultaneously. */
#if defined(_WIN32)
static INIT_ONCE g_registry_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK registry_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    g_registry_lock = fr_mutex_create();
    g_id_lock = fr_mutex_create();
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_mutexes, 0, sizeof(g_mutexes));
    return TRUE;
}

static void registry_init(void) {
    InitOnceExecuteOnce(&g_registry_once, registry_init_once, NULL, NULL);
}
#else
static pthread_once_t g_registry_once = PTHREAD_ONCE_INIT;

static void registry_init_once(void) {
    g_registry_lock = fr_mutex_create();
    g_id_lock = fr_mutex_create();
    memset(g_threads, 0, sizeof(g_threads));
    memset(g_mutexes, 0, sizeof(g_mutexes));
}

static void registry_init(void) {
    pthread_once(&g_registry_once, registry_init_once);
}
#endif

/* Acquire a live reference to the mutex behind `handle`, bumping its
 * refcount so a concurrent destroy() cannot free it out from under the
 * caller. Must be paired with mutex_ref_release(). */
static fr_mutex_t *mutex_ref_acquire(int64_t handle) {
    if (handle < 0 || handle >= THREAD_MAX_MUTEXES) return NULL;
    registry_init();
    fr_mutex_lock(g_registry_lock);
    fr_mutex_t *m = NULL;
    if (g_mutexes[handle].mutex && !g_mutexes[handle].pending_destroy) {
        m = g_mutexes[handle].mutex;
        g_mutexes[handle].refcount++;
    }
    fr_mutex_unlock(g_registry_lock);
    return m;
}

/* Release a reference taken by mutex_ref_acquire(). If destroy() was
 * requested while this reference was outstanding and this was the last
 * reference, actually destroy the underlying mutex now. */
static void mutex_ref_release(int64_t handle) {
    if (handle < 0 || handle >= THREAD_MAX_MUTEXES) return;
    fr_mutex_lock(g_registry_lock);
    if (g_mutexes[handle].refcount > 0) g_mutexes[handle].refcount--;
    if (g_mutexes[handle].pending_destroy && g_mutexes[handle].refcount == 0 &&
        g_mutexes[handle].mutex) {
        fr_mutex_destroy(g_mutexes[handle].mutex);
        g_mutexes[handle].mutex = NULL;
        g_mutexes[handle].pending_destroy = 0;
    }
    fr_mutex_unlock(g_registry_lock);
}

int64_t fr_threading_cpu_count(void) {
    return (int64_t)fr_platform_cpu_count();
}

void fr_threading_yield(void) {
    fr_thread_yield();
}

void fr_threading_pin(int64_t cpu) {
    fr_thread_pin_cpu((int)cpu);
}

int64_t fr_threading_self(void) {
#if defined(_WIN32)
    return (int64_t)(uintptr_t)GetCurrentThreadId();
#elif defined(FORGE_OS_LINUX)
    return (int64_t)(uintptr_t)pthread_self();
#else
    registry_init();
    fr_mutex_lock(g_id_lock);
    static __thread int64_t cached = 0;
    if (cached == 0) cached = ++g_next_thread_id;
    int64_t id = cached;
    fr_mutex_unlock(g_id_lock);
    return id;
#endif
}

int64_t fr_threading_worker_id(void) {
    return tls_worker_id;
}

int64_t fr_threading_mutex_create(void) {
    registry_init();
    fr_mutex_lock(g_registry_lock);
    int64_t handle = -1;
    for (int i = 0; i < THREAD_MAX_MUTEXES; i++) {
        if (!g_mutexes[i].mutex) {
            fr_mutex_t *m = fr_mutex_create();
            if (m) {
                g_mutexes[i].mutex = m;
                g_mutexes[i].refcount = 0;
                g_mutexes[i].pending_destroy = 0;
                handle = i;
            }
            break;
        }
    }
    fr_mutex_unlock(g_registry_lock);
    return handle;
}

void fr_threading_mutex_lock(int64_t handle) {
    fr_mutex_t *m = mutex_ref_acquire(handle);
    if (!m) return;
    /* The reference stays outstanding (refcount held) until the matching
     * fr_threading_mutex_unlock() call releases it, so a concurrent
     * destroy() cannot free this mutex while it is locked. */
    fr_mutex_lock(m);
}

void fr_threading_mutex_unlock(int64_t handle) {
    if (handle < 0 || handle >= THREAD_MAX_MUTEXES) return;
    registry_init();
    fr_mutex_lock(g_registry_lock);
    fr_mutex_t *m = g_mutexes[handle].mutex;
    fr_mutex_unlock(g_registry_lock);
    if (!m) return;
    fr_mutex_unlock(m);
    mutex_ref_release(handle);
}

void fr_threading_mutex_destroy(int64_t handle) {
    if (handle < 0 || handle >= THREAD_MAX_MUTEXES) return;
    registry_init();
    fr_mutex_lock(g_registry_lock);
    if (g_mutexes[handle].mutex) {
        if (g_mutexes[handle].refcount > 0) {
            /* Still referenced by an in-flight lock()/unlock() pair; defer
             * the actual destroy until the last reference is released. */
            g_mutexes[handle].pending_destroy = 1;
        } else {
            fr_mutex_destroy(g_mutexes[handle].mutex);
            g_mutexes[handle].mutex = NULL;
        }
    }
    fr_mutex_unlock(g_registry_lock);
}

typedef struct {
    fr_threading_fn1_t fn;
    int64_t arg;
} thread_arg1_t;

typedef struct {
    fr_threading_fn2_t fn;
    int64_t id;
    int64_t total;
} thread_arg2_t;

static void *spawn_trampoline(void *p) {
    thread_arg1_t *a = (thread_arg1_t *)p;
    fr_threading_fn1_t fn = a->fn;
    int64_t arg = a->arg;
    free(a);
    fn(arg);
    return NULL;
}

static void *indexed_trampoline(void *p) {
    thread_arg2_t *a = (thread_arg2_t *)p;
    fr_threading_fn2_t fn = a->fn;
    int64_t id = a->id;
    int64_t total = a->total;
    free(a);
    tls_worker_id = id;
    fn(id, total);
    tls_worker_id = -1;
    return NULL;
}

static int64_t alloc_thread_slot(fr_thread_t *t) {
    registry_init();
    fr_mutex_lock(g_registry_lock);
    int64_t handle = -1;
    for (int i = 0; i < THREAD_MAX_HANDLES; i++) {
        if (!g_threads[i].in_use) {
            g_threads[i].thread = t;
            g_threads[i].in_use = 1;
            handle = i;
            break;
        }
    }
    fr_mutex_unlock(g_registry_lock);
    return handle;
}

static fr_thread_t *take_thread_slot(int64_t handle) {
    if (handle < 0 || handle >= THREAD_MAX_HANDLES) return NULL;
    registry_init();
    fr_mutex_lock(g_registry_lock);
    fr_thread_t *t = NULL;
    if (g_threads[handle].in_use) {
        t = g_threads[handle].thread;
        g_threads[handle].thread = NULL;
        g_threads[handle].in_use = 0;
    }
    fr_mutex_unlock(g_registry_lock);
    return t;
}

int fr_threading_use_scheduler_pool(void) {
    return fr_sched_pool_available();
}

int64_t fr_threading_spawn(fr_threading_fn1_t fn, int64_t arg) {
    if (!fn) return -1;
    if (fr_sched_pool_available()) return fr_sched_pool_spawn((fr_sched_native_fn1_t)fn, arg);
    thread_arg1_t *ctx = (thread_arg1_t *)malloc(sizeof(thread_arg1_t));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    fr_thread_t *t = NULL;
    if (fr_thread_start(&t, spawn_trampoline, ctx) != 0) {
        free(ctx);
        return -1;
    }
    int64_t handle = alloc_thread_slot(t);
    if (handle < 0) {
        fr_thread_detach(t);
        return -1;
    }
    return handle;
}

void fr_threading_spawn_indexed(fr_threading_fn2_t fn, int64_t count) {
    if (!fn || count <= 0) return;
    if (fr_sched_pool_available()) {
        fr_sched_pool_spawn_indexed((fr_sched_native_fn2_t)fn, count);
        return;
    }
    if (count > THREAD_MAX_HANDLES) count = THREAD_MAX_HANDLES;

    fr_thread_t **threads = (fr_thread_t **)calloc((size_t)count, sizeof(fr_thread_t *));
    if (!threads) return;

    int started = 0;
    for (int64_t i = 0; i < count; i++) {
        thread_arg2_t *ctx = (thread_arg2_t *)malloc(sizeof(thread_arg2_t));
        if (!ctx) break;
        ctx->fn = fn;
        ctx->id = i;
        ctx->total = count;
        if (fr_thread_start(&threads[started], indexed_trampoline, ctx) != 0) {
            free(ctx);
            break;
        }
        started++;
    }

    for (int i = 0; i < started; i++) {
        if (threads[i]) fr_thread_join(threads[i]);
    }
    free(threads);
}

int64_t fr_threading_join(int64_t handle) {
    fr_thread_t *t = take_thread_slot(handle);
    if (!t) return -1;
    return fr_thread_join(t) == 0 ? 0 : -1;
}

void fr_threading_join_all(void) {
    registry_init();
    fr_mutex_lock(g_registry_lock);
    for (int i = 0; i < THREAD_MAX_HANDLES; i++) {
        if (g_threads[i].in_use && g_threads[i].thread) {
            fr_thread_t *t = g_threads[i].thread;
            g_threads[i].thread = NULL;
            g_threads[i].in_use = 0;
            fr_mutex_unlock(g_registry_lock);
            fr_thread_join(t);
            fr_mutex_lock(g_registry_lock);
        }
    }
    fr_mutex_unlock(g_registry_lock);
}
