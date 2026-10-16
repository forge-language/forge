/* GNU ld fault boundary for generated process programs. No private layout. */
#include "forge_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *owners[8], *rejected_state;
static int count, process_calls, destroys;
static _Thread_local int fail_spawn_allocation;
static const char *mode;
static void report(void) {
    int live=0;
    for(int i=0;i<count;i++) live+=owners[i]!=NULL;
    fprintf(stderr,"factory: live=%d destroys=%d\n",live,destroys);
}
static int failing(const char *name) {
    if(!mode){ mode=getenv("FORGE_TEST_FAIL"); if(!mode)mode="none"; atexit(report); }
    return !strcmp(mode,name);
}
static void remember(void *p){if(p)owners[count++]=p;}
fr_scheduler_t *__real_fr_scheduler_create(int);
fr_scheduler_t *__wrap_fr_scheduler_create(int n) {
    if(failing("scheduler"))return NULL;
    fr_scheduler_t *p=__real_fr_scheduler_create(n);remember(p);return p;
}
fr_process_t *__real_fr_process_create(const char *);
fr_process_t *__wrap_fr_process_create(const char *name) {
    if(failing("process")&&++process_calls==2)return NULL;
    fr_process_t *p=__real_fr_process_create(name);remember(p);return p;
}
fr_process_t *__real_fr_supervisor_create(const char *,fr_restart_policy_t);
fr_process_t *__wrap_fr_supervisor_create(const char *name,fr_restart_policy_t strategy) {
    if(failing("supervisor"))return NULL;
    fr_process_t *p=__real_fr_supervisor_create(name,strategy);remember(p);return p;
}
void __real_fr_scheduler_destroy(fr_scheduler_t *);
void __wrap_fr_scheduler_destroy(fr_scheduler_t *p){if(p)destroys++;__real_fr_scheduler_destroy(p);}
fr_coro_t *__real_fr_coro_spawn(fr_process_t *,fr_coro_fn,void *,size_t);
fr_coro_t *__wrap_fr_coro_spawn(fr_process_t *p,fr_coro_fn fn,void *state,size_t bytes) {
    if(failing("coroutine")){rejected_state=state;return NULL;}
    if(failing("coroutine-real")){rejected_state=state;fail_spawn_allocation=1;}
    fr_coro_t *c=__real_fr_coro_spawn(p,fn,state,bytes);
    fail_spawn_allocation=0;
    return c;
}
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t bytes) {
    if(fail_spawn_allocation){
        fail_spawn_allocation=0;
        fputs("factory: coroutine allocation failed\n",stderr);
        return NULL;
    }
    return __real_calloc(n,bytes);
}
void __real_free(void *);
void __wrap_free(void *p) {
    if(p){
        for(int i=0;i<count;i++)if(owners[i]==p)owners[i]=NULL;
        if(p==rejected_state){fputs("factory: rejected state freed\n",stderr);rejected_state=NULL;}
    }
    __real_free(p);
}
