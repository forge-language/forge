#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "forge/process.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define FR_POPEN _popen
#define FR_PCLOSE _pclose
#else
#define FR_POPEN popen
#define FR_PCLOSE pclose
#include <sys/wait.h>
#endif

#define FR_PROC_BUF_SIZE (1024 * 1024)
static char g_proc_output[FR_PROC_BUF_SIZE];

int64_t fr_proc_run(const char *command) {
    g_proc_output[0] = '\0';
    if (!command) return -1;

    FILE *p = FR_POPEN(command, "r");
    if (!p) return -1;

    size_t total = 0;
    size_t n;
    while (total < FR_PROC_BUF_SIZE - 1 &&
           (n = fread(g_proc_output + total, 1, FR_PROC_BUF_SIZE - 1 - total, p)) > 0) {
        total += n;
    }
    g_proc_output[total] = '\0';

    int status = FR_PCLOSE(p);
#if defined(_WIN32)
    return (int64_t)status;
#else
    if (status == -1) return -1;
    if (WIFEXITED(status)) return (int64_t)WEXITSTATUS(status);
    return -1;
#endif
}

const char *fr_proc_output(void) {
    return g_proc_output;
}
