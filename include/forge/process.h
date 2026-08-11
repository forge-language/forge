#ifndef FORGE_PROCESS_H
#define FORGE_PROCESS_H

#include <stdint.h>

int64_t fr_proc_run(const char *command);
const char *fr_proc_output(void);

#endif
