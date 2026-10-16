#include "function_index.h"
#include <assert.h>

static FnDecl *linear(FnDecl *functions, size_t count, ForgeStr name) {
    for (size_t i = 0; i < count; i++)
        if (forge_str_eq(functions[i].name, name)) return &functions[i];
    return NULL;
}

int main(void) {
    /* Exercise owners of different sizes and declaration orders. Duplicate
     * names have distinct payloads, so finding any equivalent key is not enough. */
    const size_t counts[] = {0, 1, 16, 17, 257};
    for (size_t k = 0; k < sizeof(counts)/sizeof(*counts); k++) {
        size_t count = counts[k];
        FnDecl functions[257] = {0}, original[257];
        char names[257][96];
        for (size_t i = 0; i < count; i++) {
            snprintf(names[i], sizeof(names[i]), "shared_prefix_abcdefghijklmnopqrstuvwxyz_%zu", (i*71)%113);
            functions[i].name = forge_str(names[i]);
            functions[i].ret_type.kind = i % 2 ? TY_STRING : TY_INT;
        }
        memcpy(original, functions, sizeof(functions));
        ForgeFunctionIndex index = forge_function_index_build(functions, count);
        for (size_t i = 0; i < count; i++)
            assert(forge_function_index_get(&index, functions[i].name) == linear(functions, count, functions[i].name));
        const char *missing[] = {"", "shared_prefix_abcdefghijklmnopqrstuvwxyz_", "zzzz", "shared_prefix_abcdefghijklmnopqrstuvwxyz_1000"};
        for (size_t i = 0; i < sizeof(missing)/sizeof(*missing); i++)
            assert(forge_function_index_get(&index, forge_str(missing[i])) == NULL);
        assert(memcmp(original, functions, sizeof(functions)) == 0);
        forge_function_index_free(&index);
    }

    /* Keys are slices, not necessarily NUL terminated. Owner indexes cannot
     * share state even when the same name resolves to different declarations. */
    char data[] = {'a','b','c','x'};
    FnDecl left[17] = {0}, right[17] = {0};
    for (size_t i = 0; i < 17; i++) {
        left[i].name = (ForgeStr){data, 3};
        right[i].name = (ForgeStr){data, 3};
    }
    ForgeFunctionIndex a = forge_function_index_build(left, 17);
    ForgeFunctionIndex b = forge_function_index_build(right, 17);
    assert(forge_function_index_get(&a, forge_str("abc")) == &left[0]);
    assert(forge_function_index_get(&b, forge_str("abc")) == &right[0]);
    assert(!forge_function_index_get(&a, forge_str("abcx")));
    for (unsigned i = 0; i < 6; i++) {
        assert(!forge_function_index_get(&a, forge_str("missing")));
        assert(!forge_function_index_get(&b, forge_str("missing")));
    }
    forge_function_index_free(&a);
    assert(forge_function_index_get(&b, forge_str("abc")) == &right[0]);
    forge_function_index_free(&b);
    return 0;
}
