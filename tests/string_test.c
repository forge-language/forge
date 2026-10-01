#include "forge/string.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int64_t v = fr_str_view("한글😀");
    assert(v && fr_str_view_len(v) == 10);
    assert(fr_str_view_at(v, 0) == 237);
    assert(fr_str_view_at(v, 9) == 128);
    assert(fr_str_view_at(v, -1) == -1);
    assert(fr_str_view_at(v, INT64_MAX) == -1);
    assert(fr_str_view_len(0) == 0 && fr_str_view_at(0, 0) == -1);
    assert(fr_str_view_len(fr_str_view(NULL)) == 0);
    const char nul_string[] = {'a', 0, 'b', 0};
    assert(fr_str_view_len(fr_str_view(nul_string)) == 1);
    int64_t b = fr_str_builder();
    assert(b && fr_str_builder_append(b, NULL) == b);
    assert(strcmp(fr_str_builder_finish(b), "") == 0);
    int64_t nul_builder = fr_str_builder();
    assert(fr_str_builder_append(nul_builder, nul_string) == nul_builder);
    assert(strcmp(fr_str_builder_finish(nul_builder), "a") == 0);
    assert(fr_str_builder_append(b, "한글") == b);
    char *snapshot = fr_str_builder_finish(b);
    int bytes[] = {240, 159, 152, 128};
    for (size_t i = 0; i < 4; ++i) assert(fr_str_builder_char(b, bytes[i]) == b);
    assert(strcmp(fr_str_builder_finish(b), "한글😀") == 0);
    assert(strcmp(snapshot, "한글") == 0);
    assert(fr_str_builder_char(b, 0) == 0);
    assert(fr_str_builder_char(b, -1) == 0);
    assert(fr_str_builder_char(b, 256) == 0);
    assert(strcmp(fr_str_builder_finish(b), "한글😀") == 0);
    assert(fr_str_builder_finish(0) == NULL);
    assert(fr_str_builder_append(0, "x") == 0);
    assert(fr_str_builder_char(0, 65) == 0);
    for (int i = 0; i < 100000; ++i) assert(fr_str_builder_char(b, 65) == b);
    char *large = fr_str_builder_finish(b);
    assert(strlen(large) == 100010 && large[100009] == 'A');
    assert(strcmp(snapshot, "한글") == 0);
    /* The old substring API must clamp without overflowing start+len. */
    assert(strcmp(fr_str_sub("abc", 1, INT64_MAX), "bc") == 0);
    fr_str_arena_reset();
    b = fr_str_builder();
    assert(fr_str_builder_append(b, "after reset") == b);
    assert(strcmp(fr_str_builder_finish(b), "after reset") == 0);
    puts("string view/builder tests passed");
    return 0;
}
