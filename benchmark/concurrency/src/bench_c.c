/* C concurrency-primitive benchmark: the raw OS-thread baseline. Spawn N
 * pthreads, each calls sched_yield() once and then increments a shared atomic
 * counter; join all N. This is what the lightweight primitives are being
 * compared against. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_llong counter;

static void *unit(void *arg) {
    (void)arg;
    sched_yield();
    atomic_fetch_add(&counter, 1);
    return NULL;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bench_c <n>\n"); return 2; }
    long n = strtol(argv[1], NULL, 10);
    if (n <= 0) { fprintf(stderr, "bad n\n"); return 2; }

    pthread_t *ts = (pthread_t *)malloc((size_t)n * sizeof(pthread_t));
    if (!ts) { fprintf(stderr, "FAIL: malloc of %ld pthread_t failed\n", n); return 1; }

    double t0 = now_ms();
    long created = 0;
    for (long i = 0; i < n; i++) {
        int rc = pthread_create(&ts[i], NULL, unit, NULL);
        if (rc != 0) {
            /* Thread-creation failure is a result, not a crash: report how far
             * we got and the actual errno string. */
            fprintf(stderr, "FAIL: pthread_create failed at thread %ld of %ld: %s\n",
                    i, n, strerror(rc));
            for (long j = 0; j < i; j++) pthread_join(ts[j], NULL);
            fprintf(stderr, "created=%ld counter=%lld\n", i, (long long)atomic_load(&counter));
            free(ts);
            return 1;
        }
        created++;
    }
    for (long i = 0; i < created; i++) pthread_join(ts[i], NULL);
    double elapsed = now_ms() - t0;

    long long c = atomic_load(&counter);
    printf("counter=%lld elapsed_ms=%.3f\n", c, elapsed);
    free(ts);
    if (c != n) { fprintf(stderr, "FAIL: counter %lld != n %ld\n", c, n); return 1; }
    return 0;
}
