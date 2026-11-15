#include "ownership.h"

size_t forge_ownership_bind(ForgeOwnership *p, bool initialized) {
    p->bindings = forge_array_reserve(p->bindings, p->count, &p->capacity,
        sizeof(*p->bindings), 16);
    p->bindings[p->count] = (ForgeOwnershipBinding){
        initialized ? FORGE_OWN_LIVE : FORGE_OWN_UNINITIALIZED, false};
    return p->count++;
}
bool forge_ownership_live(const ForgeOwnership *p, size_t binding) {
    return p->bindings[binding].state == FORGE_OWN_LIVE;
}
bool forge_ownership_owned(const ForgeOwnership *p, size_t binding) {
    return p->bindings[binding].owned;
}
void forge_ownership_qualify(ForgeOwnership *p, size_t binding, bool owned) {
    p->bindings[binding].owned = owned;
}
void forge_ownership_set_live(ForgeOwnership *p, size_t binding, bool live) {
    p->bindings[binding].state = live ? FORGE_OWN_LIVE :
        p->bindings[binding].owned ? FORGE_OWN_MOVED : FORGE_OWN_UNINITIALIZED;
}
void forge_ownership_join_add(ForgeOwnershipJoin *join, ForgeOwnershipBinding binding,
                              bool live, unsigned exits) {
    unsigned retained = binding.owned ? FLOW_NEXT|FLOW_BREAK|FLOW_CONTINUE : FLOW_NEXT;
    if (!(exits & retained)) return;
    join->live = join->reachable ? join->live && live : live;
    join->reachable = true;
}
void forge_ownership_join_finish(ForgeOwnership *p, size_t binding,
                                 ForgeOwnershipJoin join, bool before) {
    forge_ownership_set_live(p, binding, join.reachable ? join.live : before);
}
void forge_ownership_loop_join(ForgeOwnership *p, size_t binding, bool before,
                               bool after) {
    /* Include the zero-iteration path; a possible move survives loop exit. */
    forge_ownership_set_live(p, binding, before && after);
}
void forge_ownership_free(ForgeOwnership *p) {
    free(p->bindings);
    *p = (ForgeOwnership){0};
}
