#ifndef FORGE_PROCESS_H
#define FORGE_PROCESS_H

#include <stdint.h>

int64_t fr_proc_run(const char *command);
int64_t fr_proc_run_forge(const char *forge, const char *file, const char *flag,
                          const char *forge_root, const char *lib_dir,
                          const char *include_paths);
const char *fr_proc_output(void);

#endif
