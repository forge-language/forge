/* Shared observable counter + internal timer for the Forge concurrency
 * benchmark, linked into the Forge binary through Forge's `extern fn` FFI.
 *
 * Forge (0.3.0) has no module-level variables, no atomics, and its
 * `on receive(...)` process handler is parsed but never emitted by codegen,
 * so a Forge program cannot by itself hold a counter shared between
 * coroutines nor observe when the last coroutine has finished. The other
 * languages in this benchmark all do exactly what this file does using
 * their own standard library (atomic add / WaitGroup / join / gather).
 *
 * bc_start(n)  - record N and the start timestamp (called right before the
 *                spawn loop, so process startup is excluded).
 * bc_bump()    - one unit of observable work per concurrency unit; when the
 *                count reaches N, stamp the completion time.
 * destructor   - print "counter=<c> elapsed_ms=<t>" once, at process exit.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static atomic_llong bc_count;
static long long bc_expected;
static long long bc_t0_ns;
static atomic_llong bc_t1_ns;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t bc_start(int64_t n) {
    bc_expected = (long long)n;
    atomic_store(&bc_count, 0);
    atomic_store(&bc_t1_ns, 0);
    bc_t0_ns = now_ns();
    return n;
}

int64_t bc_bump(void) {
    long long c = atomic_fetch_add(&bc_count, 1) + 1;
    if (c == bc_expected) atomic_store(&bc_t1_ns, now_ns());
    return c;
}

__attribute__((destructor)) static void bc_report(void) {
    if (bc_expected == 0) return;
    long long t1 = atomic_load(&bc_t1_ns);
    if (t1 == 0) t1 = now_ns();
    printf("counter=%lld elapsed_ms=%.3f\n",
           (long long)atomic_load(&bc_count),
           (double)(t1 - bc_t0_ns) / 1e6);
    fflush(stdout);
}
