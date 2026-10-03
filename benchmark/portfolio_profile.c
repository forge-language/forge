#define _POSIX_C_SOURCE 200809L
#include "forge_web.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Linked only into the disposable diagnostic executable with --wrap. */
#define PROFILE_WORKERS 512
typedef struct {
    atomic_uint_fast64_t requests, handler_ns, cpu_ns;
    atomic_uint_fast64_t acquire_ns, query_ns, queries;
    atomic_uint_fast64_t worker_requests[PROFILE_WORKERS];
} Metrics;
static Metrics metrics[4];
static fw_handler original_handler;
static atomic_uint workers_seen;
static _Thread_local int worker_index = -1;
static _Thread_local int in_request;
static _Thread_local uint64_t acquire_ns, query_ns, queries;

static uint64_t clock_ns(clockid_t clock) {
    struct timespec value;
    if (clock_gettime(clock, &value)) abort();
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

extern int64_t __real_fpg_acquire(int64_t);
extern int64_t __real_fpg_query_prepared(int64_t, const char *, int64_t);
extern int64_t __real_fpg_query(int64_t, const char *, int64_t);
extern int64_t __real_fw_run(const char *, int64_t, int64_t, fw_handler);

int64_t __wrap_fpg_acquire(int64_t pool) {
    if (!in_request) return __real_fpg_acquire(pool);
    uint64_t start = clock_ns(CLOCK_MONOTONIC);
    int64_t result = __real_fpg_acquire(pool);
    acquire_ns += clock_ns(CLOCK_MONOTONIC) - start;
    return result;
}

int64_t __wrap_fpg_query_prepared(int64_t conn, const char *sql, int64_t params) {
    if (!in_request) return __real_fpg_query_prepared(conn, sql, params);
    uint64_t start = clock_ns(CLOCK_MONOTONIC);
    int64_t result = __real_fpg_query_prepared(conn, sql, params);
    query_ns += clock_ns(CLOCK_MONOTONIC) - start;
    queries++;
    return result;
}

int64_t __wrap_fpg_query(int64_t conn, const char *sql, int64_t params) {
    if (!in_request) return __real_fpg_query(conn, sql, params);
    uint64_t start = clock_ns(CLOCK_MONOTONIC);
    int64_t result = __real_fpg_query(conn, sql, params);
    query_ns += clock_ns(CLOCK_MONOTONIC) - start;
    queries++;
    return result;
}

static int64_t measured_handler(int64_t request) {
    const char *path = fw_path(request);
    if (!strcmp(path, "/__forge_diagnosis_metrics")) {
        char body[32768];
        size_t used = 0;
        const char *names[] = {"health", "posts", "projects", "other"};
        body[used++] = '{';
        for (size_t i = 0; i < 4; i++) {
            Metrics *m = &metrics[i];
            int written = snprintf(body + used, sizeof(body) - used,
                "%s\"%s\":{\"requests\":%" PRIu64 ",\"handler_ns\":%" PRIu64
                ",\"cpu_ns\":%" PRIu64 ",\"acquire_ns\":%" PRIu64
                ",\"query_ns\":%" PRIu64 ",\"queries\":%" PRIu64
                ",\"worker_requests\":[",
                i ? "," : "", names[i],
                (uint64_t)atomic_load(&m->requests), (uint64_t)atomic_load(&m->handler_ns),
                (uint64_t)atomic_load(&m->cpu_ns), (uint64_t)atomic_load(&m->acquire_ns),
                (uint64_t)atomic_load(&m->query_ns), (uint64_t)atomic_load(&m->queries));
            if (written < 0 || (size_t)written >= sizeof(body) - used - 2) abort();
            used += (size_t)written;
            for (size_t j = 0; j < PROFILE_WORKERS; j++) {
                written = snprintf(body + used, sizeof(body) - used,
                    "%s%" PRIu64, j ? "," : "",
                    (uint64_t)atomic_load(&m->worker_requests[j]));
                if (written < 0 || (size_t)written >= sizeof(body) - used - 4) abort();
                used += (size_t)written;
            }
            body[used++] = ']'; body[used++] = '}';
        }
        body[used++] = '}'; body[used] = 0;
        return fw_respond(request, 200, body);
    }
    int index = !strcmp(path, "/api/health") ? 0 :
                !strcmp(path, "/api/posts") ? 1 :
                !strcmp(path, "/api/projects") ? 2 : 3;
    if (worker_index < 0) {
        unsigned id = atomic_fetch_add(&workers_seen, 1);
        worker_index = id < PROFILE_WORKERS ? (int)id : PROFILE_WORKERS - 1;
    }
    uint64_t start = clock_ns(CLOCK_MONOTONIC);
    uint64_t cpu_start = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    acquire_ns = query_ns = queries = 0;
    in_request = 1;
    int64_t result = original_handler(request);
    in_request = 0;
    Metrics *m = &metrics[index];
    atomic_fetch_add(&m->handler_ns, clock_ns(CLOCK_MONOTONIC) - start);
    atomic_fetch_add(&m->cpu_ns, clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start);
    atomic_fetch_add(&m->acquire_ns, acquire_ns);
    atomic_fetch_add(&m->query_ns, query_ns);
    atomic_fetch_add(&m->queries, queries);
    atomic_fetch_add(&m->worker_requests[worker_index], 1);
    atomic_fetch_add(&m->requests, 1);
    return result;
}

int64_t __wrap_fw_run(const char *host, int64_t port, int64_t workers, fw_handler handler) {
    const char *configured = getenv("FORGE_DIAG_WORKERS");
    if (configured) {
        char *end;
        long count = strtol(configured, &end, 10);
        if (!*configured || *end || count < 1 || count > 64) return 1;
        workers = count;
    }
    original_handler = handler;
    return __real_fw_run(host, port, workers, measured_handler);
}
