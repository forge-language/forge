#include "parser.h"
#include "semantic.h"
#include "optimize.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)

static Program parse(const char *source) {
    Lexer lexer;
    lexer_init(&lexer,source,strlen(source));
    return parse_program_named(&lexer,"typed-test.fg");
}
static bool named(const Expr *e, const char *name) {
    return e->kind == EXPR_IDENT && forge_str_eq(e->as.ident,forge_str(name));
}
static void contextual_types(void) {
    const char *source =
        "import opaque; import strings; const BASE=1; const VALUE=BASE;"
        "extern fn consume(cb:int):int; fn callback(x:int):int{return x;}"
        "native main {let outer:int=VALUE;"
        "{let BASE:float=2.5;let inner:float=VALUE;println(inner);}"
        "let BASE:string=\"shadow\";let text:string=VALUE;"
        "let result:float=helper.value();let opaque_result:int=opaque.value();"
        "consume(callback);print_int(outer);str_len(text);return 0;}";
    Program p=parse(source);
    p.module_count=1;
    p.modules=forge_calloc_array(1,sizeof(*p.modules));
    p.modules[0].name=(ForgeStr){forge_strdup(forge_str("helper")),6};
    p.modules[0].fn_count=1;
    p.modules[0].functions=forge_calloc_array(1,sizeof(FnDecl));
    p.modules[0].functions[0]=(FnDecl){.name=forge_str("value"),
        .ret_type=forge_type_float(),.is_extern=true};
    forge_check_program(&p);
    optimize_program(&p);
    ForgeTypedProgram t=forge_type_program(&p);
    CHECK(forge_typed_verify(&t));
    size_t counts[3]={0}, bindings[2]={FORGE_TYPED_NONE,FORGE_TYPED_NONE};
    bool module=false, callback=false, opaque=false, void_call=false, standard=false;
    size_t local_node=FORGE_TYPED_NONE;
    for (size_t i=0;i<t.expression_count;++i) {
        ForgeTypedExpr *e=&t.expressions[i];
        if (named(e->origin,"VALUE")) {
            CHECK(e->constant == &p.consts[1]);
            CHECK(e->type.kind == FORGE_TYPED_VALUE);
            CHECK(e->has_expected);
            CHECK(e->span.file && e->span.end > e->span.start);
            if (e->type.value.kind==TY_INT) counts[0]++;
            else if (e->type.value.kind==TY_FLOAT) counts[1]++;
            else if (e->type.value.kind==TY_STRING) counts[2]++;
            else CHECK(false);
            CHECK(i+1<t.expression_count && t.expressions[i+1].parent==i);
            CHECK(t.expressions[i+1].origin==p.consts[1].value);
        }
        if (named(e->origin,"BASE") && e->binding!=FORGE_TYPED_NONE) {
            CHECK(e->origin==p.consts[1].value);
            CHECK(t.bindings[e->binding].type.kind==e->type.value.kind);
            if (e->type.value.kind==TY_FLOAT) bindings[0]=e->binding;
            if (e->type.value.kind==TY_STRING) bindings[1]=e->binding;
            local_node=i;
        }
        if (e->call==FORGE_CALL_SOURCE && e->function==&p.modules[0].functions[0]) {
            CHECK(e->type.kind==FORGE_TYPED_VALUE && e->type.value.kind==TY_FLOAT); module=true;
        }
        if (e->type.kind==FORGE_TYPED_FUNCTION) {
            CHECK(e->function==&p.functions[1]);
            CHECK(e->function->params && e->function->ret_type.kind==TY_INT); callback=true;
        }
        if (e->call==FORGE_CALL_OPAQUE) {
            CHECK(e->type.kind==FORGE_TYPED_UNKNOWN);
            CHECK(e->has_expected && e->expected.kind==TY_INT); opaque=true;
        }
        if (e->call==FORGE_CALL_BUILTIN && e->type.value.kind==TY_VOID) void_call=true;
        if (e->call==FORGE_CALL_STANDARD) { CHECK(e->standard); standard=true; }
    }
    CHECK(counts[0]==1 && counts[1]==1 && counts[2]==1);
    CHECK(bindings[0]!=FORGE_TYPED_NONE && bindings[1]!=FORGE_TYPED_NONE && bindings[0]!=bindings[1]);
    CHECK(t.bindings[bindings[0]].scope!=t.bindings[bindings[1]].scope);
    CHECK(module && callback && opaque && void_call && standard);
    ForgeTypedProgram malformed=t;
    malformed.expression_count=t.expression_capacity+1;
    CHECK(!forge_typed_verify(&malformed));
    malformed=t; malformed.binding_count=t.binding_capacity+1;
    CHECK(!forge_typed_verify(&malformed));
    malformed=t; malformed.scope_count=t.scope_capacity+1;
    CHECK(!forge_typed_verify(&malformed));
    malformed=t; malformed.expressions=NULL;
    CHECK(!forge_typed_verify(&malformed));
    ForgeTypedExpr saved=t.expressions[local_node];
    t.expressions[local_node].binding=t.binding_count;
    CHECK(!forge_typed_verify(&t));
    t.expressions[local_node]=saved;
    t.expressions[local_node].parent=local_node;
    CHECK(!forge_typed_verify(&t));
    t.expressions[local_node]=saved;
    t.expressions[local_node].type.kind=(ForgeTypedKind)99;
    CHECK(!forge_typed_verify(&t));
    t.expressions[local_node]=saved;
    t.expressions[local_node].span.end=source ? strlen(source)+1 : 0;
    CHECK(!forge_typed_verify(&t));
    t.expressions[local_node]=saved;
    CHECK(forge_typed_verify(&t));
    forge_typed_free(&t);
    CHECK(!t.expressions && !t.bindings && !t.scope_parents);
    program_free(&p);
    puts("PASS contextual constants, shadow IDs, module/callback/opaque/void/SDK types, verifier negative controls");
}
static void optimized_graph(void) {
    Program p=parse("const FOLD=1*(2+3);native main{false && (1/0==0);return FOLD;}");
    forge_check_program(&p);
    Expr *surviving=p.consts[0].value->as.binary.right;
    optimize_program(&p);
    CHECK(p.consts[0].value==surviving && surviving->kind==EXPR_INT);
    ForgeTypedProgram t=forge_type_program(&p);
    CHECK(forge_typed_verify(&t));
    size_t folded=0, trap=0, short_circuit=0;
    for (size_t i=0;i<t.expression_count;++i) {
        ForgeTypedExpr *e=&t.expressions[i];
        if (e->origin==surviving) {CHECK(e->kind==EXPR_INT); folded++;}
        if (e->effects&FORGE_TYPED_MAY_TRAP) {CHECK(e->kind==EXPR_BINARY); trap++;}
        if (e->effects&FORGE_TYPED_SHORT_CIRCUIT) short_circuit++;
    }
    CHECK(folded==2 && trap==1 && short_circuit==1);
    forge_typed_free(&t);
    program_free(&p);
    puts("PASS optimizer parent/child replacement, per-use expansion, trap and short-circuit metadata");
}
static void flow_reinterpretation(void) {
    Program p=parse("process main{own let s:string=\"payload\";"
        "if (1==1) {send 0,Drop,move s;return;} else {} println(s);}");
    forge_check_program(&p);
    optimize_program(&p);
    ForgeTypedProgram t=forge_type_program(&p);
    CHECK(forge_typed_verify(&t));
    forge_typed_free(&t);
    program_free(&p);
    puts("PASS post-optimization literal branch and owned-move join retain precheck acceptance");
}
static void callback_resolution(void) {
    Program p=parse("const callback=9;fn callback(x:int):int{return x;}"
        "extern fn consume(cb:int):int;native main{"
        "thread_spawn(callback,1);consume(helper.work);return 0;}");
    p.module_count=1;
    p.modules=forge_calloc_array(1,sizeof(*p.modules));
    p.modules[0].name=(ForgeStr){forge_strdup(forge_str("helper")),6};
    p.modules[0].fn_count=1;
    p.modules[0].functions=forge_calloc_array(1,sizeof(FnDecl));
    p.modules[0].functions[0]=(FnDecl){.name=forge_str("work"),
        .ret_type=forge_type_int(),.is_extern=true};
    forge_check_program(&p);
    optimize_program(&p);
    ForgeTypedProgram t=forge_type_program(&p);
    CHECK(forge_typed_verify(&t));
    size_t callbacks=0;
    for (size_t i=0;i<t.expression_count;++i) {
        ForgeTypedExpr *e=&t.expressions[i];
        if (e->type.kind!=FORGE_TYPED_FUNCTION) continue;
        if (e->kind==EXPR_IDENT) {
            CHECK(e->function==&p.functions[0] && !e->constant);
        } else {
            CHECK(e->kind==EXPR_FIELD && e->function==&p.modules[0].functions[0]);
        }
        callbacks++;
    }
    CHECK(callbacks==2);
    forge_typed_free(&t);
    program_free(&p);
    puts("PASS builtin callback ignores same-name constant; qualified callback target");
}
static void annotation_growth(void) {
    Program p=parse("fn identity(x:float):float{return x;}native main{return 0;}");
    /* Force many nested calls across the graph's allocation boundaries. */
    Stmt *ret=p.natives[0].body.first;
    Expr *value=ret->as.ret;
    for (size_t i=0;i<96;++i) {
        Expr **args=forge_calloc_array(1,sizeof(*args));
        args[0]=value;
        value=expr_call(forge_str("identity"),args,1);
    }
    ret->as.ret=value;
    /* Native returns require int, so inspect as an expression statement. */
    ret->kind=STMT_EXPR;
    ret->as.expr=value;
    forge_check_program(&p);
    optimize_program(&p);
    ForgeTypedProgram t=forge_type_program(&p);
    CHECK(forge_typed_verify(&t));
    size_t expected=0;
    for (size_t i=0;i<t.expression_count;++i)
        if (t.expressions[i].has_expected) {
            CHECK(t.expressions[i].expected.kind==TY_FLOAT);
            expected++;
        }
    CHECK(expected==97);
    forge_typed_free(&t);
    program_free(&p);
    puts("PASS expected types survive nested expression-array growth");
}
int main(void) {
    contextual_types();
    callback_resolution();
    annotation_growth();
    optimized_graph();
    flow_reinterpretation();
    return 0;
}
