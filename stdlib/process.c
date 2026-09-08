#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "forge/process.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define FR_POPEN _popen
#define FR_PCLOSE _pclose
#include <io.h>
#include <process.h>
#else
#define FR_POPEN popen
#define FR_PCLOSE pclose
#include <sys/wait.h>
#include <unistd.h>
#endif

#define FR_PROC_BUF_SIZE (1024 * 1024)
static char g_proc_output[FR_PROC_BUF_SIZE];

static void fr_proc_append_output(const char *buf, size_t *total, size_t n) {
    size_t available = FR_PROC_BUF_SIZE - 1 - *total;
    if (n > available) n = available;
    if (n > 0) {
        memcpy(g_proc_output + *total, buf, n);
        *total += n;
    }
}

static int fr_proc_finish_status(int status) {
#if defined(_WIN32)
    return status < 0 ? -1 : status;
#else
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

int64_t fr_proc_run(const char *command) {
    g_proc_output[0] = '\0';
    if (!command) return -1;

    FILE *p = FR_POPEN(command, "r");
    if (!p) return -1;

    size_t total = 0;
    size_t n;
    char buf[4096];
    /* Drain even after capture is full, otherwise pclose waits for a child
     * blocked writing to the full pipe. */
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
        fr_proc_append_output(buf, &total, n);
    g_proc_output[total] = '\0';

    return (int64_t)fr_proc_finish_status(FR_PCLOSE(p));
}

/* Run the compiler with an argument vector instead of passing user-controlled
 * paths through a shell. include_paths is a newline-delimited list of paths. */
int64_t fr_proc_run_forge(const char *forge, const char *file, const char *flag,
                          const char *forge_root, const char *lib_dir,
                          const char *include_paths) {
    g_proc_output[0] = '\0';
    if (!forge || !file || !flag) return -1;

    const char *paths = include_paths ? include_paths : "";
    size_t path_count = 0;
    for (const char *p = paths; *p; p++) {
        if (p == paths || p[-1] == '\n') path_count++;
    }

    size_t argc = 4 + (forge_root && *forge_root ? 2 : 0) +
                  (lib_dir && *lib_dir ? 2 : 0) + path_count * 2;
    char **argv = (char **)calloc(argc + 1, sizeof(char *));
    if (!argv) return -1;

    size_t i = 0;
    argv[i++] = (char *)forge;
    argv[i++] = (char *)file;
    argv[i++] = (char *)flag;
    if (forge_root && *forge_root) {
        argv[i++] = "--forge-root";
        argv[i++] = (char *)forge_root;
    }
    if (lib_dir && *lib_dir) {
        argv[i++] = "--lib-dir";
        argv[i++] = (char *)lib_dir;
    }
    size_t first_path_arg = i;
    const char *start = paths;
    for (const char *p = paths;; p++) {
        if (*p != '\n' && *p != '\0') continue;
        if (p > start) {
            size_t len = (size_t)(p - start);
            char *path = (char *)malloc(len + 1);
            if (!path) {
                for (size_t j = first_path_arg + 1; j < i; j += 2)
                    free(argv[j]);
                free(argv);
                return -1;
            }
            memcpy(path, start, len);
            path[len] = '\0';
            argv[i++] = "-I";
            argv[i++] = path;
        }
        if (*p == '\0') break;
        start = p + 1;
    }
    argv[i] = NULL;

    int status = -1;
#if defined(_WIN32)
    int saved_out = _dup(_fileno(stdout));
    int saved_err = _dup(_fileno(stderr));
    FILE *capture = tmpfile();
    fflush(stdout);
    fflush(stderr);
    if (saved_out >= 0 && saved_err >= 0 && capture &&
        _dup2(_fileno(capture), _fileno(stdout)) == 0 &&
        _dup2(_fileno(capture), _fileno(stderr)) == 0) {
        status = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
        fflush(stdout);
        fflush(stderr);
        _dup2(saved_out, _fileno(stdout));
        _dup2(saved_err, _fileno(stderr));
        if (fseek(capture, 0, SEEK_SET) == 0) {
            char buf[4096];
            size_t total = 0;
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), capture)) > 0)
                fr_proc_append_output(buf, &total, n);
            g_proc_output[total] = '\0';
        }
    }
    /* Restore descriptors even when only the first redirection succeeded. */
    if (saved_out >= 0) _dup2(saved_out, _fileno(stdout));
    if (saved_err >= 0) _dup2(saved_err, _fileno(stderr));
    if (capture) fclose(capture);
    if (saved_out >= 0) _close(saved_out);
    if (saved_err >= 0) _close(saved_err);
#else
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            execvp(argv[0], argv);
            _exit(127);
        }
        close(pipefd[1]);
        if (pid > 0) {
            char buf[4096];
            size_t total = 0;
            ssize_t n;
            do {
                n = read(pipefd[0], buf, sizeof(buf));
                if (n > 0) fr_proc_append_output(buf, &total, (size_t)n);
            } while (n > 0 || (n < 0 && errno == EINTR));
            close(pipefd[0]);
            int raw_status = 0;
            pid_t waited;
            do {
                waited = waitpid(pid, &raw_status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited >= 0)
                status = fr_proc_finish_status(raw_status);
            g_proc_output[total] = '\0';
        } else {
            close(pipefd[0]);
        }
    }
#endif

    for (size_t j = first_path_arg + 1; j < i; j += 2)
        free(argv[j]);
    free(argv);
    return (int64_t)status;
}

const char *fr_proc_output(void) {
    return g_proc_output;
}
