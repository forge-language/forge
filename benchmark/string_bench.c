#define _POSIX_C_SOURCE 200809L
#include "forge/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(1);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static volatile unsigned long long checksum;
int main(void) {
    puts("operation,implementation,bytes,seconds,checksum");
    const size_t sizes[] = {4096, 16384, 65536};
    for (size_t k = 0; k < sizeof(sizes)/sizeof(sizes[0]); ++k) {
        size_t n = sizes[k];
        char *s = malloc(n + 1);
        if (!s) return 1;
        memset(s, 'x', n); s[n] = 0;
        for (int mode = 0; mode < 2; ++mode) {
            unsigned long long sum = 0;
            fr_str_arena_reset();
            double start = now();
            int64_t view = mode ? fr_str_view(s) : 0;
            for (size_t i = 0; i < n; ++i)
                sum += mode ? fr_str_view_at(view, (int64_t)i) : fr_str_char_at(s, (int64_t)i);
            double elapsed = now() - start; checksum = sum;
            printf("scan,%s,%zu,%.9f,%llu\n", mode ? "view" : "legacy", n, elapsed, sum);
        }
        /* Legacy immutable append stores quadratic data in its arena; cap
         * this row at 16KiB to avoid a benchmark allocating gigabytes. */
        if (n <= 16384) for (int mode = 0; mode < 2; ++mode) {
            fr_str_arena_reset();
            double start = now();
            char *result = "";
            int64_t b = mode ? fr_str_builder() : 0;
            for (size_t i = 0; i < n; ++i) {
                if (mode) { if (!fr_str_builder_char(b, 'x')) return 1; }
                else { result = fr_str_append(result, 'x'); if (!result) return 1; }
            }
            if (mode) result = fr_str_builder_finish(b);
            double elapsed = now() - start;
            if (!result || strlen(result) != n || memcmp(result, s, n)) return 1;
            checksum = strlen(result);
            printf("append,%s,%zu,%.9f,%zu\n", mode ? "builder" : "legacy", n, elapsed, strlen(result));
        }
        free(s);
    }
    fr_str_arena_reset();
    return checksum == 0;
}
