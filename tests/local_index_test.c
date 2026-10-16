#include "local_index.h"
#include <assert.h>

typedef struct { ForgeStr name; unsigned value; bool initialized; } Binding;
typedef struct { Binding *items; size_t count, capacity; ForgeLocalIndex index; } Stack;

static void push(Stack *s, ForgeStr name, unsigned value) {
    s->items = forge_array_reserve(s->items, s->count, &s->capacity, sizeof(*s->items), 16);
    s->items[s->count++] = (Binding){name, value, true};
    forge_local_push(&s->index, name);
}

static void pop(Stack *s, size_t count) {
    forge_local_truncate(&s->index, count);
    s->count = count;
}

static void query(Stack *s, ForgeStr name) {
    size_t expected = SIZE_MAX;
    for (size_t i = s->count; i; i--)
        if (forge_str_eq(s->items[i-1].name, name)) { expected = i-1; break; }
    assert(forge_local_find(&s->index, s->items, s->count, sizeof(*s->items), name) == expected);
}

int main(void) {
    Stack s = {0};
    char names[256][48], colliders[48][48];
    for (unsigned i = 0; i < 256; i++) snprintf(names[i], sizeof(names[i]), "binding_%u", i);
    /* Deliberate bucket collisions must preserve both distinct names and LIFO
     * shadowing. Expected answers always come from an independent reverse scan. */
    unsigned found = 0;
    for (unsigned i = 0; found < 48; i++) {
        char text[48]; snprintf(text, sizeof(text), "collision_%u", i);
        if ((forge_local_hash(forge_str(text)) & 1023) == 0)
            strcpy(colliders[found++], text);
    }
    query(&s, forge_str("missing"));
    for (unsigned i = 0; i < 192; i++) push(&s, forge_str(names[i]), i);
    for (unsigned i = 0; i < 12; i++) query(&s, forge_str("missing"));
    /* Force owner storage to move after cache activation. */
    Binding *moved = malloc(s.capacity * sizeof(*moved));
    assert(moved); memcpy(moved, s.items, s.count * sizeof(*moved)); free(s.items); s.items = moved;
    for (unsigned i = 0; i < 48; i++) push(&s, forge_str(colliders[i]), i);
    for (unsigned i = 0; i < 48; i++) query(&s, forge_str(colliders[i]));
    size_t outer = s.count;
    for (unsigned i = 0; i < 48; i++) push(&s, forge_str(colliders[i]), 1000+i);
    for (unsigned i = 0; i < 48; i++) query(&s, forge_str(colliders[i]));
    pop(&s, outer);
    for (unsigned i = 0; i < 48; i++) query(&s, forge_str(colliders[i]));
    pop(&s, 0);

    /* Repeated scopes, slot reuse, growth and flow-state changes. No query is
     * allowed to return a popped declaration or a stale owner pointer. */
    uint64_t random = 20261015;
    for (unsigned step = 0; step < 40000; step++) {
        random = random * UINT64_C(6364136223846793005) + 1;
        unsigned name = (unsigned)(random >> 32) % 256;
        if ((random & 7) == 0 && s.count) pop(&s, (size_t)(random >> 32) % s.count);
        else if ((random & 3) == 0 || !s.count) push(&s, forge_str(names[name]), step);
        else query(&s, forge_str(names[name]));
        if (s.count > 1024) pop(&s, 33);
        if (s.count) s.items[name % s.count].initialized = false;
    }
    pop(&s, 0);
    /* Popped names may die. A fresh, smaller owner reuses buffers without ever
     * hashing the stale keys or clearing the previous high-water capacity. */
    char *owned[1024];
    for (unsigned i = 0; i < 1024; i++) {
        owned[i] = forge_strdup(forge_str(names[i % 256])); assert(owned[i]);
        push(&s, forge_str(owned[i]), i);
    }
    for (unsigned i = 0; i < 12; i++) query(&s, forge_str("missing"));
    pop(&s, 0);
    for (unsigned i = 0; i < 1024; i++) free(owned[i]);
    for (unsigned i = 0; i < 40; i++) push(&s, forge_str(names[i]), i);
    for (unsigned i = 0; i < 12; i++) query(&s, forge_str(names[0]));
    pop(&s, 0);
    char a[] = {'q',0,'a'}, b[] = {'q',0,'b'};
    for (unsigned i = 0; i < 40; i++) push(&s, forge_str(names[i]), i);
    push(&s, (ForgeStr){a,3}, 1);
    push(&s, (ForgeStr){b,3}, 2);
    for (unsigned i = 0; i < 12; i++) query(&s, forge_str("missing"));
    query(&s, (ForgeStr){a,3}); query(&s, (ForgeStr){b,3});
    pop(&s, s.count-1); query(&s, (ForgeStr){b,3});
    pop(&s, 0);
    forge_local_free(&s.index);
    free(s.items);
    return 0;
}
