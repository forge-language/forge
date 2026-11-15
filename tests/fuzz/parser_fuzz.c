#include "parser.h"
#include "semantic.h"
#include "optimize.h"
#include "safety.h"
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern uint8_t __start___sancov_cntrs[], __stop___sancov_cntrs[];
static pid_t owner;
static uint8_t *coverage;
static size_t coverage_size;
_Noreturn void __real_exit(int status);
static _Noreturn void finish_child(int status) {
    memcpy(coverage,__start___sancov_cntrs,coverage_size);
    _exit(status);
}
_Noreturn void __wrap_exit(int status) {
    if (owner && getpid()!=owner) finish_child(status);
    __real_exit(status);
}
int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    owner=getpid();
    coverage_size=(size_t)(__stop___sancov_cntrs-__start___sancov_cntrs);
    coverage=mmap(NULL,coverage_size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if (coverage==MAP_FAILED) abort();
    return 0;
}

/* Parser diagnostics intentionally terminate the CLI. Isolate each input so
 * ordinary rejection is distinct from sanitizer errors, signals and hangs. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size>65536) return 0;
    int output[2];
    if (pipe(output)!=0) abort();
    memcpy(coverage,__start___sancov_cntrs,coverage_size);
    pid_t child=fork();
    if (child<0) abort();
    if (child==0) {
        close(output[0]);
        if (dup2(output[1],STDERR_FILENO)<0) _exit(99);
        close(output[1]);
        struct rlimit cpu={1,1}, core={0,0};
        if (setrlimit(RLIMIT_CPU,&cpu)!=0 || setrlimit(RLIMIT_CORE,&core)!=0) _exit(99);
        alarm(2);
        Lexer lexer;
        lexer_init(&lexer,(const char *)data,size);
        Program program=parse_program_named(&lexer,"fuzz.fg");
        forge_check_program(&program);
        optimize_program(&program);
        ForgeTypedProgram typed=forge_type_program(&program);
        if (!forge_typed_verify(&typed)) abort();
        forge_check_safety(&program,&typed);
        forge_typed_free(&typed);
        program_free(&program);
        finish_child(0);
    }
    close(output[1]);
    char report[8192], discard[4096];
    size_t used=0;
    for (;;) {
        char *destination=used<sizeof(report) ? report+used : discard;
        size_t capacity=used<sizeof(report) ? sizeof(report)-used : sizeof(discard);
        ssize_t n=read(output[0],destination,capacity);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        if (used<sizeof(report)) used+=(size_t)n;
    }
    close(output[0]);
    int status;
    while (waitpid(child,&status,0)<0) { if (errno!=EINTR) abort(); }
    if (!WIFEXITED(status) || (WEXITSTATUS(status)!=0 && WEXITSTATUS(status)!=1)) {
        if (used) fwrite(report,1,used,stderr);
        abort();
    }
    memcpy(__start___sancov_cntrs,coverage,coverage_size);
    return 0;
}
