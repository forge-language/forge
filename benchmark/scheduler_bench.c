#define _POSIX_C_SOURCE 200809L
#include "forge_runtime.h"
#include "forge/event.h"
#include "forge/thread.h"
#include <stdio.h>
#include <stdlib.h>
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
int main(int argc, char **argv) {
    unsigned ms = 500;
    if (argc > 2) return 1;
    if (argc == 2) {
        char *end;
        unsigned long value = strtoul(argv[1], &end, 10);
        if (*end || value < 50 || value > 10000) return 1;
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
