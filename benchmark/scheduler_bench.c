#define _POSIX_C_SOURCE 200809L
#include "forge_runtime.h"
#include "forge/event.h"
#include "forge/thread.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { int fd; unsigned milliseconds; } io_state_t;
static void *writer(void *arg) {
    io_state_t *s = arg;
    struct timespec delay = {s->milliseconds / 1000, (s->milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
    if (write(s->fd, "x", 1) != 1) exit(1);
    return NULL;
}
static fr_coro_status_t reader(fr_coro_t *coro, void *arg) {
    io_state_t *s = arg;
    int64_t ready = fr_await_fd(coro, s->fd, FR_EVENT_READ);
    if (ready < 0) exit(1);
    if (!ready) return FR_CORO_WAITING_IO;
    char c;
    if (read(s->fd, &c, 1) != 1) exit(1);
    return FR_CORO_DONE;
}
static double elapsed(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1000000.0;
}

typedef struct { atomic_size_t *completed; int yielded; } task_state_t;
static fr_coro_status_t task(fr_coro_t *coro, void *arg) {
    task_state_t *s = arg;
    if (!s->yielded) { s->yielded = 1; return fr_yield(coro); }
    atomic_fetch_add_explicit(s->completed, 1, memory_order_relaxed);
    return FR_CORO_DONE;
}
static int throughput(size_t count, int workers) {
    atomic_size_t completed = 0;
    fr_scheduler_t *sched = fr_scheduler_create(workers);
    fr_process_t *proc = fr_process_create("throughput");
    if (!sched || !proc) return 1;
    fr_scheduler_add_process(sched, proc);
    for (size_t i = 0; i < count; i++) {
        task_state_t *state = malloc(sizeof(*state));
        if (!state) return 1;
        *state = (task_state_t){&completed, 0};
        if (!fr_coro_spawn(proc, task, state, sizeof(*state))) return 1;
    }
    struct timespec cpu_start, cpu_end, wall_start, wall_end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    fr_scheduler_run(sched);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    size_t result = atomic_load(&completed);
    printf("coroutines,workers,wall_ms,process_cpu_ms,completed\n%zu,%d,%.3f,%.3f,%zu\n",
           count, workers, elapsed(wall_start, wall_end), elapsed(cpu_start, cpu_end), result);
    fr_scheduler_destroy(sched);
    return result != count;
}

static unsigned long number(const char *text, unsigned long min, unsigned long max) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value < min || value > max) return 0;
    return value;
}
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "throughput") == 0) {
        if (argc != 4) return 1;
        unsigned long count = number(argv[2], 1, 1000000);
        unsigned long workers = number(argv[3], 1, 32);
        return count && workers ? throughput(count, (int)workers) : 1;
    }
    unsigned ms = 500;
    if (argc > 2) return 1;
    if (argc == 2) {
        unsigned long value = number(argv[1], 50, 10000);
        if (!value) return 1;
        ms = (unsigned)value;
    }
    int fds[2];
    if (pipe(fds)) return 1;
    fr_scheduler_t *sched = fr_scheduler_create(2);
    fr_process_t *proc = fr_process_create("idle-io");
    if (!sched || !proc) return 1;
    fr_scheduler_add_process(sched, proc);
    io_state_t *state = malloc(sizeof(*state));
    if (!state) return 1;
    *state = (io_state_t){fds[0], ms};
    if (!fr_coro_spawn(proc, reader, state, sizeof(*state))) return 1;
    io_state_t write_state = {fds[1], ms};
    fr_thread_t *thread;
    struct timespec cpu_start, cpu_end, wall_start, wall_end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    if (fr_thread_start(&thread, writer, &write_state)) return 1;
    fr_scheduler_run(sched);
    fr_thread_join(thread);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    printf("wait_ms,wall_ms,process_cpu_ms\n%u,%.3f,%.3f\n", ms,
           elapsed(wall_start, wall_end), elapsed(cpu_start, cpu_end));
    fr_scheduler_destroy(sched);
    close(fds[0]); close(fds[1]);
    return 0;
}
