#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#include "forge/os.h"
#include "forge/arena.h"
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#define COMMAND_ARG_LIMIT 256
#define COMMAND_TEXT_LIMIT 65536
typedef struct {
    size_t argc;
    char *argv[COMMAND_ARG_LIMIT + 2];
} os_command_t;

/* Bounded inspection avoids allocating for unexpectedly large source strings. */
static char *command_copy(const char *text) {
    if (!text) return NULL;
    size_t len = 0;
    while (len <= COMMAND_TEXT_LIMIT && text[len]) len++;
    if (len > COMMAND_TEXT_LIMIT) return NULL;
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, text, len + 1);
    return copy;
}

int64_t fr_os_command(const char *program) {
    if (!program || !*program) return 0;
    os_command_t *command = calloc(1, sizeof(*command));
    if (!command) return 0;
    command->argv[0] = command_copy(program);
    if (!command->argv[0]) { free(command); return 0; }
    command->argc = 1;
    return (int64_t)(intptr_t)command;
}

int64_t fr_os_command_arg(int64_t handle, const char *arg) {
    os_command_t *command = (os_command_t *)(intptr_t)handle;
    if (!command || command->argc > COMMAND_ARG_LIMIT) return 0;
    char *copy = command_copy(arg);
    if (!copy) return 0;
    command->argv[command->argc++] = copy;
    return 1;
}

void fr_os_command_free(int64_t handle) {
    os_command_t *command = (os_command_t *)(intptr_t)handle;
    if (!command) return;
    for (size_t i = 0; i < command->argc; i++) free(command->argv[i]);
    free(command);
}

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) return NULL;
    wchar_t *out = malloc((size_t)count * sizeof(*out));
    if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, count)) {
        free(out);
        out = NULL;
    }
    return out;
}

static char *wide_arena_path(const wchar_t *path) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                    NULL, 0, NULL, NULL);
    if (!count) return NULL;
    char *out = fr_arena_alloc(fr_arena_tls(), (size_t)count, 1);
    if (!out || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                    out, count, NULL, NULL)) return NULL;
    return out;
}

/* Quote every argument using the Microsoft C runtime argv rules. Backslashes
 * double before quotes and the closing quote; elsewhere they remain literal. */
static wchar_t *command_line(const os_command_t *command) {
    const size_t cap = 32767;
    wchar_t *line = malloc(cap * sizeof(*line));
    if (!line) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < command->argc; i++) {
        wchar_t *arg = utf8_to_wide(command->argv[i]);
        if (!arg) { free(line); return NULL; }
        size_t len = wcslen(arg);
        size_t quoted = 2, scan = 0;
        while (scan < len) {
            size_t slashes = 0;
            while (scan < len && arg[scan] == L'\\') { slashes++; scan++; }
            if (scan == len) quoted += slashes * 2;
            else if (arg[scan++] == L'"') quoted += slashes * 2 + 2;
            else quoted += slashes + 1;
        }
        size_t needed = quoted + (i ? 1 : 0);
        if (needed >= cap - used) { free(arg); free(line); return NULL; }
        if (i) line[used++] = L' ';
        line[used++] = L'"';
        size_t pos = 0;
        while (pos < len) {
            size_t slashes = 0;
            while (pos < len && arg[pos] == L'\\') { slashes++; pos++; }
            if (pos == len || arg[pos] == L'"') {
                for (size_t j = 0; j < slashes * 2; j++) line[used++] = L'\\';
                if (pos < len) { line[used++] = L'\\'; line[used++] = arg[pos++]; }
            } else {
                for (size_t j = 0; j < slashes; j++) line[used++] = L'\\';
                line[used++] = arg[pos++];
            }
        }
        line[used++] = L'"';
        free(arg);
    }
    line[used] = L'\0';
    return line;
}
#endif

int64_t fr_os_command_run(int64_t handle) {
    os_command_t *command = (os_command_t *)(intptr_t)handle;
    if (!command) return 126;
#ifdef _WIN32
    wchar_t *line = command_line(command);
    if (!line) return 126;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    BOOL ok = CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    DWORD error = ok ? 0 : GetLastError();
    free(line);
    if (!ok) return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 127 : 126;
    CloseHandle(process.hThread);
    DWORD code = 126;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int64_t)code;
#else
    pid_t child;
    int rc;
    do {
        rc = posix_spawnp(&child, command->argv[0], NULL, NULL, command->argv, environ);
    } while (rc == EINTR);
    if (rc != 0) return rc == ENOENT ? 127 : 126;
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) return 126;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 126;
#endif
}

char *fr_os_temp_file(void) {
#ifdef _WIN32
    wchar_t directory[MAX_PATH], path[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, directory);
    if (!len || len >= MAX_PATH || !GetTempFileNameW(directory, L"frg", 0, path)) return NULL;
    char *out = wide_arena_path(path);
    if (!out) DeleteFileW(path);
    return out;
#else
    char path[] = "/tmp/forge-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    /* Retrying close after EINTR can close an unrelated descriptor on Linux. */
    if (close(fd) < 0 && errno != EINTR) { unlink(path); return NULL; }
    char *out = fr_arena_strdup(fr_arena_tls(), path);
    if (!out) unlink(path);
    return out;
#endif
}

char *fr_os_executable_path(void) {
#ifdef _WIN32
    for (DWORD cap = 256; cap <= 32768; cap *= 2) {
        wchar_t *path = malloc((size_t)cap * sizeof(*path));
        if (!path) return NULL;
        DWORD len = GetModuleFileNameW(NULL, path, cap);
        if (len && len < cap) {
            char *out = wide_arena_path(path);
            free(path);
            return out;
        }
        free(path);
        if (!len) return NULL;
    }
    return NULL;
#elif defined(__linux__)
    for (size_t cap = 256; cap <= 1024 * 1024; cap *= 2) {
        char *path = malloc(cap + 1);
        if (!path) return NULL;
        ssize_t len = readlink("/proc/self/exe", path, cap);
        if (len >= 0 && (size_t)len < cap) {
            path[len] = '\0';
            char *out = fr_arena_strdup(fr_arena_tls(), path);
            free(path);
            return out;
        }
        free(path);
        if (len < 0) return NULL;
    }
    return NULL;
#elif defined(__APPLE__)
    uint32_t cap = 0;
    _NSGetExecutablePath(NULL, &cap);
    if (!cap) return NULL;
    char *path = malloc(cap);
    if (!path) return NULL;
    char *out = NULL;
    if (_NSGetExecutablePath(path, &cap) == 0) {
        char *resolved = realpath(path, NULL);
        if (resolved) { out = fr_arena_strdup(fr_arena_tls(), resolved); free(resolved); }
    }
    free(path);
    return out;
#else
    return NULL;
#endif
}

int64_t fr_os_same_file(const char *left, const char *right) {
    if (!left || !right) return 0;
#ifdef _WIN32
    wchar_t *left_path = utf8_to_wide(left), *right_path = utf8_to_wide(right);
    if (!left_path || !right_path) { free(left_path); free(right_path); return 0; }
    DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE a = CreateFileW(left_path, FILE_READ_ATTRIBUTES, shared, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE b = CreateFileW(right_path, FILE_READ_ATTRIBUTES, shared, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(left_path);
    free(right_path);
    BY_HANDLE_FILE_INFORMATION ai, bi;
    int same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a, &ai) && GetFileInformationByHandle(b, &bi) &&
        ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber &&
        ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    return same;
#else
    struct stat a, b;
    return stat(left, &a) == 0 && stat(right, &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
}

static int g_argc = 0;
static char **g_argv = NULL;

void fr_os_set_args(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

void fr_os_exit(int64_t code) {
    exit((int)code);
}

const char *fr_os_getenv(const char *name) {
    if (!name) return "";
    const char *v = getenv(name);
    return v ? v : "";
}

int64_t fr_os_argc(void) {
    return g_argc;
}

const char *fr_os_argv(int64_t index) {
    if (index < 0 || index >= g_argc || !g_argv) return "";
    return g_argv[index] ? g_argv[index] : "";
}
