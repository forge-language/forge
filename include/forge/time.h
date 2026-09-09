#ifndef FORGE_TIME_H
#define FORGE_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t fr_time_now_ms(void);
void fr_sleep_ms(int64_t ms);

#ifdef __cplusplus
}
#endif

#endif
