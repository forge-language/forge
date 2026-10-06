#include "forge_runtime.h"
#include "forge/platform.h"

int main(void) {
    fr_scheduler_t *scheduler = fr_scheduler_create(1);
    if (!scheduler) return 1;
    fr_arena_t *arena = fr_arena_create(64);
    if (!arena) { fr_scheduler_destroy(scheduler); return 2; }
    int failed = !fr_arena_alloc(arena, 32, 8) || fr_platform_cpu_count() < 1;
    fr_arena_destroy(arena);
    fr_scheduler_destroy(scheduler);
    return failed;
}
