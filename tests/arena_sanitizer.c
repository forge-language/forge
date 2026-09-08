#include "forge/arena.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    fr_arena_t *arena = fr_arena_create(4096);
    assert(arena);
    void *addresses[16];
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int i = 0; i < 16; i++) {
            void *p = fr_arena_alloc(arena, 3000, 64);
            assert(p && (uintptr_t)p % 64 == 0);
            if (cycle == 0) addresses[i] = p;
            else assert(p == addresses[i]);
            memset(p, i, 3000);
        }
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 3000; j++)
                assert(((unsigned char *)addresses[i])[j] == i);
        fr_arena_reset(arena);
    }
    fr_arena_destroy(arena);
    return 0;
}
