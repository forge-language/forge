#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "forge/os.h"
#include "forge/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static const char *literal_args[] = {
    "", "two words", "semi;colon", "$(echo substituted)", "`echo substituted`",
    "a&b|c>d<e", "*.fg", "quote\"inside", "single'quote", "\\",
    "trailing\\", "space and trailing\\", "slash\\\"quote", "line\nfeed", "tab\there"
};

#ifndef _WIN32
static volatile sig_atomic_t wait_interruptions;
static void interrupt_wait(int signal_number) {
    (void)signal_number;
    wait_interruptions++;
}
#endif

static int child(int argc, char **argv) {
    if (!strcmp(argv[1], "--literal-child")) {
        size_t count = sizeof(literal_args) / sizeof(literal_args[0]);
        if (argc != (int)count + 2) return 61;
        for (size_t i = 0; i < count; i++)
            if (strcmp(argv[i + 2], literal_args[i])) return 62;
        return 0;
    }
    if (!strcmp(argv[1], "--exit-child")) return 37;
    if (!strcmp(argv[1], "--long-child")) {
        if (argc != 3 || strlen(argv[2]) != 8192) return 63;
        for (size_t i = 0; i < 8192; i++) if (argv[2][i] != 'x') return 64;
        return 0;
    }
    if (!strcmp(argv[1], "--count-child")) return argc == 257 ? 0 : 65;
#ifndef _WIN32
    if (!strcmp(argv[1], "--signal-child")) { raise(SIGTERM); return 66; }
    if (!strcmp(argv[1], "--wait-child")) {
        struct timespec pause = {0, 80000000};
        nanosleep(&pause, NULL);
        return 23;
    }
#endif
    return 67;
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argc, argv);
    CHECK(fr_os_command(NULL) == 0);
    CHECK(fr_os_command("") == 0);
    CHECK(fr_os_command_arg(0, "arg") == 0);
    CHECK(fr_os_command_run(0) == 126);
    fr_os_command_free(0);
    const char *self = fr_os_executable_path();
    CHECK(self && *self);
    struct stat info;
    CHECK(stat(self, &info) == 0);
    char *program = malloc(strlen(self) + 1);
    CHECK(program);
    strcpy(program, self);
    int64_t handle = fr_os_command(program);
    CHECK(handle);
    memset(program, '!', strlen(program));
    free(program);
    CHECK(fr_os_command_arg(handle, "--literal-child") == 1);
    for (size_t i = 0; i < sizeof(literal_args) / sizeof(literal_args[0]); i++) {
        char *arg = malloc(strlen(literal_args[i]) + 1);
        CHECK(arg);
        strcpy(arg, literal_args[i]);
        CHECK(fr_os_command_arg(handle, arg) == 1);
        memset(arg, '!', strlen(arg));
        free(arg);
    }
    CHECK(fr_os_command_run(handle) == 0);
    CHECK(fr_os_command_run(handle) == 0);
    fr_os_command_free(handle);

    handle = fr_os_command(self);
    CHECK(handle && fr_os_command_arg(handle, "--exit-child"));
    CHECK(fr_os_command_run(handle) == 37);
    fr_os_command_free(handle);
    handle = fr_os_command("__forge_missing_executable_6877cbf07__");
    CHECK(handle && fr_os_command_run(handle) == 127);
    fr_os_command_free(handle);
#ifndef _WIN32
    handle = fr_os_command(self);
    CHECK(handle && fr_os_command_arg(handle, "--signal-child"));
    CHECK(fr_os_command_run(handle) == 128 + SIGTERM);
    fr_os_command_free(handle);
    struct sigaction action = {0}, old_action;
    action.sa_handler = interrupt_wait;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, &old_action) == 0);
    struct itimerval timer = {{0, 10000}, {0, 10000}}, stopped = {{0, 0}, {0, 0}};
    handle = fr_os_command(self);
    CHECK(handle && fr_os_command_arg(handle, "--wait-child"));
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(fr_os_command_run(handle) == 23);
    CHECK(setitimer(ITIMER_REAL, &stopped, NULL) == 0);
    CHECK(sigaction(SIGALRM, &old_action, NULL) == 0);
    CHECK(wait_interruptions > 0);
    fr_os_command_free(handle);
#endif

    char *large = malloc(65538);
    CHECK(large);
    memset(large, 'x', 65537);
    large[65537] = '\0';
    CHECK(fr_os_command(large) == 0);
    handle = fr_os_command(self);
    CHECK(handle && fr_os_command_arg(handle, NULL) == 0);
    CHECK(fr_os_command_arg(handle, large) == 0);
    CHECK(fr_os_command_arg(handle, "--long-child"));
    large[8192] = '\0';
    CHECK(fr_os_command_arg(handle, large));
    CHECK(fr_os_command_run(handle) == 0);
    fr_os_command_free(handle);
    large[8192] = 'x';
    large[65536] = '\0';
    handle = fr_os_command(self);
    CHECK(handle && fr_os_command_arg(handle, large) == 1);
    fr_os_command_free(handle);
    free(large);

    handle = fr_os_command(self);
    CHECK(handle && fr_os_command_arg(handle, "--count-child"));
    for (int i = 0; i < 255; i++) CHECK(fr_os_command_arg(handle, "argument"));
    CHECK(fr_os_command_arg(handle, "overflow") == 0);
    CHECK(fr_os_command_run(handle) == 0);
    fr_os_command_free(handle);

    char *temp = fr_os_temp_file(), *second = fr_os_temp_file();
    CHECK(temp && second && strcmp(temp, second));
    CHECK(fr_os_same_file(NULL, temp) == 0 && fr_os_same_file(temp, NULL) == 0);
    CHECK(fr_os_same_file(temp, temp) == 1);
    CHECK(fr_os_same_file(temp, second) == 0);
#ifndef _WIN32
    char *alias = malloc(strlen(temp) + 6);
    CHECK(alias);
    sprintf(alias, "%s.link", temp);
    CHECK(link(temp, alias) == 0);
    CHECK(fr_os_same_file(temp, alias) == 1);
    CHECK(remove(alias) == 0);
    CHECK(symlink(temp, alias) == 0);
    CHECK(fr_os_same_file(temp, alias) == 1);
    CHECK(remove(alias) == 0);
    free(alias);
#endif
    CHECK(stat(temp, &info) == 0 && info.st_size == 0);
    FILE *file = fopen(temp, "wb");
    CHECK(file && fwrite("native temp", 1, 11, file) == 11 && fclose(file) == 0);
    CHECK(stat(temp, &info) == 0 && info.st_size == 11);
#ifndef _WIN32
    handle = fr_os_command(temp);
    CHECK(handle && fr_os_command_run(handle) == 126);
    /* An executable text file without a shebang must not fall back to a shell. */
    CHECK(chmod(temp, 0700) == 0);
    CHECK(fr_os_command_run(handle) == 126);
    fr_os_command_free(handle);
#endif
    CHECK(remove(temp) == 0 && remove(second) == 0);
    CHECK(stat(temp, &info) != 0);
    CHECK(fr_os_same_file(temp, temp) == 0);
    fr_arena_tls_reset();
    puts("os tests passed");
    return 0;
}
