#include "work_queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { REPETITIONS = 5 };

static void native_task(void *arg) {
    (void)arg;
}

static struct timespec now(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) exit(1);
    return ts;
}

static double elapsed_ns(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) * 1000000000.0 +
           (double)(end.tv_nsec - start.tv_nsec);
}

static double bench_run_steal(size_t count) {
    char token;
    double best = 0.0;
    for (int repetition = 0; repetition < REPETITIONS; repetition++) {
        fr_run_queue_t victim;
        fr_run_queue_t thief;
        fr_run_queue_init(&victim);
        fr_run_queue_init(&thief);
        for (size_t i = 0; i < count; i++)
            fr_run_queue_push(&victim, (fr_coro_t *)&token);
        struct timespec start = now();
        for (size_t i = 0; i < count; i++) {
            if (!fr_run_queue_steal(&victim, &thief)) exit(1);
        }
        double elapsed = elapsed_ns(start, now());
        if (repetition == 0 || elapsed < best) best = elapsed;
        if (victim.count != 0 || fr_run_queue_steal(&victim, &thief)) exit(1);
        fr_run_queue_destroy(&thief);
        fr_run_queue_destroy(&victim);
    }
    return best / (double)count;
}

static double bench_native_steal(size_t count) {
    double best = 0.0;
    for (int repetition = 0; repetition < REPETITIONS; repetition++) {
        fr_native_queue_t victim;
        fr_native_queue_init(&victim);
        for (size_t i = 0; i < count; i++)
            fr_native_queue_push(&victim, native_task, NULL);
        struct timespec start = now();
        for (size_t i = 0; i < count; i++) {
            if (fr_native_queue_steal(&victim, NULL) != native_task) exit(1);
        }
        double elapsed = elapsed_ns(start, now());
        if (repetition == 0 || elapsed < best) best = elapsed;
        if (victim.count != 0 || fr_native_queue_steal(&victim, NULL)) exit(1);
        fr_native_queue_destroy(&victim);
    }
    return best / (double)count;
}

int main(int argc, char **argv) {
    size_t largest = 100000;
    if (argc > 2) return 1;
    if (argc == 2) {
        char *end;
        errno = 0;
        unsigned long long n = strtoull(argv[1], &end, 10);
        if (errno || *end || n < 10000 || n > 1000000) return 1;
        largest = (size_t)n;
    }
    const size_t sizes[] = {1000, 10000, largest};
    puts("queue,size,best_ns_per_steal");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        printf("run,%zu,%.2f\n", sizes[i], bench_run_steal(sizes[i]));
        printf("native,%zu,%.2f\n", sizes[i], bench_native_steal(sizes[i]));
    }
    return 0;
}
