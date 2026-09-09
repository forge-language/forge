/* Toolchain devex benchmark: spawn 10000 lightweight units of work that
 * each yield once, then join them all. C has no built-in coroutines, so
 * pthreads (with a small stack) stand in as the idiomatic "unit of
 * concurrent work" a C programmer would reach for here. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define WORKERS 10000

static void *tick(void *arg) {
    (void)arg;
    sched_yield();
    return NULL;
}

int main(void) {
    pthread_t *threads = malloc(sizeof(pthread_t) * WORKERS);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);

    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&threads[i], &attr, tick, NULL) != 0) {
            fprintf(stderr, "pthread_create failed at %d\n", i);
            return 1;
        }
    }
    for (int i = 0; i < WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_attr_destroy(&attr);
    free(threads);
    printf("done: %d\n", WORKERS);
    return 0;
}
