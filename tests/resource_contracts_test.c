#include "resource_contracts.h"
#include <assert.h>

int main(void) {
    size_t total = 0, checked = 0, opaque = 0;
    for (size_t m = 0; m < forge_std_module_count(); ++m) {
        const ForgeModule *module = forge_std_module_at(m);
        assert(module);
        for (size_t i = 0; i < module->fn_count; ++i) {
            const ForgeStdFn *fn = &module->fns[i];
            const ForgeResourceContract *c = forge_resource_contract(fn);
            ++total;
            size_t argc = strlen(fn->signature) - 1;
            assert(argc <= sizeof(c->arguments) / sizeof(*c->arguments));
            unsigned positions = (1u << argc) - 1;
            assert(((c->borrow_arguments | c->mutate_arguments | c->consume_arguments |
                     c->release_arguments) & ~positions) == 0);
            assert((c->release_arguments & c->consume_arguments) == c->release_arguments);
            if (c->opaque_reason) {
                assert(c->opaque_reason[0] && c->result == FORGE_RESOURCE_OPAQUE);
                if (c->scope_cleanup) {
                    assert(!strcmp(c->c_name,"fr_os_command"));
                    assert(c->nullable && c->allocator && c->release);
                    assert(!strcmp(c->release,"fr_os_command_free"));
                }
                ++opaque;
                continue;
            }
            ++checked;
            assert(strcmp(c->c_name, fn->c_name) == 0);
            if (c->result == FORGE_RESOURCE_STRING) assert(fn->signature[0] == 's');
            if (c->result == FORGE_RESOURCE_VIEW || c->result == FORGE_RESOURCE_BUILDER)
                assert(fn->signature[0] == 'i');
            for (size_t j = 0; j < argc; ++j) {
                if (c->arguments[j] == FORGE_RESOURCE_STRING) assert(fn->signature[j+1] == 's');
                if (c->arguments[j] == FORGE_RESOURCE_VIEW || c->arguments[j] == FORGE_RESOURCE_BUILDER)
                    assert(fn->signature[j+1] == 'i');
            }
            if (c->allocate) assert(c->allocator && c->release && c->region == FORGE_REGION_ARENA);
            if (c->region == FORGE_REGION_ARGUMENT) {
                assert(c->borrow_arguments & 1);
                assert(c->arguments[0] == c->result);
            }
        }
    }
    assert(total >= 168 && checked > 0 && opaque > 0);
    assert(forge_std_module_at(forge_std_module_count()) == NULL);
    assert(forge_resource_contract(NULL)->opaque_reason);
    printf("resource contracts: %zu SDK functions, %zu described, %zu explicitly opaque\n", total, checked, opaque);
    return 0;
}
