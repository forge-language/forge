#include "const_index.h"
#include <assert.h>

static ConstDecl *linear(ConstDecl *constants, size_t count, ForgeStr name) {
    for (size_t i = 0; i < count; ++i)
        if (forge_str_eq(constants[i].name, name)) return &constants[i];
    return NULL;
}

static uint32_t random_state = 20261015;
static uint32_t next_random(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

int main(void) {
    const size_t counts[] = {0, 1, 16, 17, 257};
    for (size_t k = 0; k < sizeof(counts)/sizeof(*counts); ++k) {
        size_t count = counts[k];
        ConstDecl constants[257] = {0}, original[257];
        Expr values[257] = {0};
        char names[257][96];
        for (size_t i = 0; i < count; ++i) {
            snprintf(names[i], sizeof(names[i]), "same_long_prefix_abcdefghijklmnopqrstuvwxyz_%zu",
                     (i * 71) % (count <= 17 ? 7 : 113));
            values[i].kind = EXPR_INT;
            values[i].as.int_val = (int64_t)i;
            constants[i] = (ConstDecl){forge_str(names[i]), &values[i]};
        }
        if (count) constants[0].name = forge_str("");
        memcpy(original, constants, sizeof(constants));
        ForgeConstIndex index = forge_const_index_build(constants, count);
        assert(index.entries == NULL);
        assert(forge_const_index_get(&index, forge_str("")) == linear(constants, count, forge_str("")));
        for (unsigned query = 0; query < 8000; ++query) {
            ForgeStr name;
            char missing[96];
            if (count && next_random() % 4) name = constants[next_random() % count].name;
            else {
                snprintf(missing, sizeof(missing), "absent_%u", next_random());
                name = forge_str(missing);
            }
            assert(forge_const_index_get(&index, name) == linear(constants, count, name));
        }
        assert(memcmp(original, constants, sizeof(constants)) == 0);
        if (count > 16) assert(index.entries);
        else assert(!index.entries);
        forge_const_index_free(&index);
        assert(!index.entries && !index.count && !index.constants);
    }

    ConstDecl constants[17] = {0}, peer[17] = {0};
    char names[17][8];
    for (size_t i = 0; i < 17; ++i) {
        snprintf(names[i], sizeof(names[i]), "K_%02zu", i);
        constants[i].name = peer[i].name = forge_str(names[i]);
    }
    ForgeConstIndex a = forge_const_index_build(constants, 17);
    ForgeConstIndex b = forge_const_index_build(peer, 17);
    /* One hit spends one comparison. Four full scans cannot complete the last
     * search's budget, so lookup must finish correctly through promotion. */
    assert(forge_const_index_get(&a, constants[0].name) == &constants[0]);
    for (int i = 0; i < 3; ++i) assert(!forge_const_index_get(&a, forge_str("missing")));
    assert(!a.entries && a.probes_left == 16);
    assert(forge_const_index_get(&a, constants[16].name) == &constants[16]);
    assert(a.entries && !b.entries);
    assert(forge_const_index_get(&b, peer[0].name) == &peer[0]);
    forge_const_index_free(&a);
    assert(forge_const_index_get(&b, peer[0].name) == &peer[0]);
    forge_const_index_free(&b);

    /* Equal-length strncmp equality ignores bytes after a NUL. Slices need
     * not be terminated, and a length difference must still be a miss. */
    char left[] = {'a', '\0', 'x'}, right[] = {'a', '\0', 'y'};
    char slice[] = {'a', 'b', 'c', 'x'};
    constants[0].name = (ForgeStr){left, 3};
    constants[1].name = (ForgeStr){right, 3};
    constants[2].name = (ForgeStr){slice, 3};
    a = forge_const_index_build(constants, 17);
    for (int i = 0; i < 6; ++i) assert(!forge_const_index_get(&a, forge_str("missing")));
    assert(a.entries);
    assert(forge_const_index_get(&a, (ForgeStr){right, 3}) == &constants[0]);
    assert(forge_const_index_get(&a, forge_str("abc")) == &constants[2]);
    assert(!forge_const_index_get(&a, forge_str("abcx")));
    forge_const_index_free(&a);
    return 0;
}
