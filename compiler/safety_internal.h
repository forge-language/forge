#ifndef FORGE_SAFETY_INTERNAL_H
#define FORGE_SAFETY_INTERNAL_H
#include "safety.h"
#include "typed_lookup.h"

#define FORGE_SAFETY_MAX_ARGS 32
#define FORGE_SAFETY_MAX_SUMMARIES 2048

typedef struct {
    ForgeResourceKind kind;
    bool arena, valid, pending;
    uint64_t parameters;
    size_t generation;
} SafetyValue;
typedef struct {
    ForgeStr name;
    SourceSpan span;
    SafetyValue value;
} SafetyBinding;
typedef struct {
    const FnDecl *function;
    ForgeResourceKind arguments[FORGE_SAFETY_MAX_ARGS];
    uint64_t arena_arguments;
    uint64_t pending_arguments;
    size_t count;
    unsigned state;
    SafetyValue result;
    bool resets;
} SafetySummary;
typedef struct {
    SafetySummary *items;
    size_t count, capacity;
    size_t work_left;
} SafetySummaries;
typedef struct SafetyLoop {
    struct SafetyLoop *parent;
    SafetyBinding *breaks, *continues;
    size_t count;
    bool has_break, has_continue;
    size_t break_generation, continue_generation;
} SafetyLoop;
typedef struct {
    Program *program;
    ForgeTypedIndex *index;
    SafetySummaries *summaries;
    SafetyBinding *bindings;
    size_t count, capacity, context, generation;
    unsigned depth;
    bool coroutine, resets, has_return;
    SafetyValue returned;
    SafetyLoop *loop;
    ProcessDecl *process;
} Safety;

void safety_error(SourceSpan, const char *);
SafetyValue safety_value(ForgeResourceKind);
SafetyValue safety_expression(Safety *, Expr *);
SafetyValue safety_call(Safety *, Expr *, const ForgeTypedExpr *);
SafetyValue safety_function(Safety *, const FnDecl *, const SafetyValue *, size_t);
bool safety_statements(Safety *, Stmt *);
bool safety_statement(Safety *, Stmt *);
bool safety_loop(Safety *, Stmt *);
void safety_loop_exit(Safety *, bool is_continue);
bool safety_block(Safety *, Block *);
size_t safety_binding(Safety *, ForgeStr);
void safety_declare(Safety *, ForgeStr, SourceSpan, SafetyValue);
void safety_reset(Safety *);
void safety_validate_value(Safety *, SafetyValue, SourceSpan);
void safety_check_pending(Safety *, size_t);
void safety_join(Safety *, const SafetyBinding *, size_t);
SafetyBinding *safety_snapshot(const Safety *, size_t);
#endif
