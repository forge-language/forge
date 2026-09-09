/* Uniform external measurement harness for every language in this benchmark.
 *
 * GNU /usr/bin/time is not installed on this machine, so this stands in for
 * `/usr/bin/time -v`: it fork/execs the child, waits with wait4(), and reports
 * ru_maxrss (the same kernel counter GNU time prints as "Maximum resident set
 * size", and the same value as VmHWM) plus monotonic wall-clock time. Using
 * one harness for all seven languages is what makes the RSS numbers
 * comparable at all.
 *
 * Also applies RLIMIT_AS to the child so a runaway at N=1000000 cannot take
 * the machine down; hitting the cap is recorded as a failure result, which is
 * itself one of the more informative outcomes.
 *
 * Usage: runner <mem_cap_mb|0> <timeout_s|0> <cmd> [args...]
 * Emits to stderr, one line:
 *   RUNNER wall_ms=<f> maxrss_kb=<d> exit=<d> signal=<d> timedout=<0|1>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static pid_t child_pid = -1;

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <mem_cap_mb|0> <timeout_s|0> <cmd> [args...]\n", argv[0]);
        return 2;
    }
    long cap_mb = strtol(argv[1], NULL, 10);
    long timeout_s = strtol(argv[2], NULL, 10);

    double t0 = now_ms();
    child_pid = fork();
    if (child_pid < 0) { perror("fork"); return 2; }

    if (child_pid == 0) {
        if (cap_mb > 0) {
            /* Virtual-address cap. Deliberately generous relative to the RSS
             * we expect, because JVM/Node/Go reserve far more address space
             * than they fault in; the point is only to stop a runaway. */
            struct rlimit rl;
            rl.rlim_cur = (rlim_t)cap_mb * 1024 * 1024;
            rl.rlim_max = (rlim_t)cap_mb * 1024 * 1024;
            setrlimit(RLIMIT_AS, &rl);
        }
        execvp(argv[3], &argv[3]);
        fprintf(stderr, "exec %s failed: %s\n", argv[3], strerror(errno));
        _exit(127);
    }

    /* Poll-with-alarm instead of a blocking wait so a hung child is killed
     * rather than wedging the whole sweep. */
    int status = 0, timedout = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    double deadline = timeout_s > 0 ? t0 + (double)timeout_s * 1000.0 : 0.0;
    for (;;) {
        pid_t r = wait4(child_pid, &status, WNOHANG, &ru);
        if (r == child_pid) break;
        if (r < 0) { if (errno == EINTR) continue; perror("wait4"); return 2; }
        if (deadline > 0.0 && now_ms() > deadline) {
            timedout = 1;
            kill(child_pid, SIGKILL);
            wait4(child_pid, &status, 0, &ru);
            break;
        }
        struct timespec nap = { 0, 2 * 1000 * 1000 };  /* 2 ms */
        nanosleep(&nap, NULL);
    }
    double wall = now_ms() - t0;

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    fprintf(stderr, "RUNNER wall_ms=%.3f maxrss_kb=%ld exit=%d signal=%d timedout=%d\n",
            wall, (long)ru.ru_maxrss, exit_code, sig, timedout);
    fflush(stderr);
    return 0;
}
