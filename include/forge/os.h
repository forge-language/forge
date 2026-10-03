#ifndef FORGE_OS_H
#define FORGE_OS_H

#include <stdint.h>

void fr_os_set_args(int argc, char **argv);
void fr_os_exit(int64_t code);
const char *fr_os_getenv(const char *name);
int64_t fr_os_argc(void);
const char *fr_os_argv(int64_t index);

/* Commands own copied program/arguments until explicit free. Handles are opaque
 * trusted values; do not reuse after free. Up to 256 additional arguments and
 * 65536 bytes per program/argument are accepted. No shell parses arguments.
 * run returns the child exit status (128 + signal on POSIX), 127 for a missing
 * executable, or 126 for another launch/wait error. Windows also enforces its
 * 32767 UTF-16-unit command-line limit and uses the standard C argv convention. */
int64_t fr_os_command(const char *program);
int64_t fr_os_command_arg(int64_t handle, const char *arg);
int64_t fr_os_command_run(int64_t handle);
void fr_os_command_free(int64_t handle);

/* Arena-owned paths, or NULL on failure. temp_file creates a closed, empty file
 * which the caller removes. executable_path supports Linux, macOS and Windows;
 * other platforms return NULL. */
char *fr_os_temp_file(void);
char *fr_os_executable_path(void);
/* Follow links and compare underlying file identity; return 0 on failure. */
int64_t fr_os_same_file(const char *left, const char *right);

#endif
