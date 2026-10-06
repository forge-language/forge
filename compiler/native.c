#define _POSIX_C_SOURCE 200809L
#include "native.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct { unsigned char *data; size_t len,cap; } Code;
enum { ELF_EHDR=64, ELF_PHDR=56, START_CODE=16 };

static bool reserve(Code *c,size_t n) {
    if(n>SIZE_MAX-c->len) return false;
    size_t need=c->len+n;
    if(need<=c->cap) return true;
    size_t cap=c->cap?c->cap:64;
    while(cap<need) { if(cap>SIZE_MAX/2) return false; cap*=2; }
    unsigned char *p=realloc(c->data,cap);
    if(!p) return false;
    c->data=p;c->cap=cap;return true;
}
static bool bytes(Code *c,const void *p,size_t n) {
    if(!reserve(c,n)) return false;
    memcpy(c->data+c->len,p,n);c->len+=n;return true;
}
static bool byte(Code *c,unsigned v) { unsigned char b=(unsigned char)v;return bytes(c,&b,1); }
static bool imm32(Code *c,uint32_t v) {
    unsigned char b[4];for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(v>>(8*i));return bytes(c,b,4);
}
static bool imm64(Code *c,uint64_t v) {
    unsigned char b[8];for(unsigned i=0;i<8;i++)b[i]=(unsigned char)(v>>(8*i));return bytes(c,b,8);
}
static bool put16(unsigned char *p,uint16_t v) { p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);return true; }
static void put32(unsigned char *p,uint32_t v) { for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i)); }
static void put64(unsigned char *p,uint64_t v) { for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(v>>(8*i)); }

static int32_t value_disp(const IRFunction *fn,IRValue v) {
    if(v==IR_NO_VALUE || v>=fn->next_value || v>INT32_MAX/8) return 0;
    return -(int32_t)((v+1)*8);
}
static int32_t local_disp(const IRFunction *fn,uint32_t local) {
    uint64_t slot=(uint64_t)fn->next_value+local+1;
    if(local>=fn->local_count || slot>INT32_MAX/8) return 0;
    return -(int32_t)(slot*8);
}
static bool emit_disp(Code *c,int32_t disp) { return imm32(c,(uint32_t)disp); }
static bool load_value(Code *c,const IRFunction *fn,IRValue v) {
    int32_t d=value_disp(fn,v);if(!d)return false;
    return byte(c,0x48)&&byte(c,0x8b)&&byte(c,0x85)&&emit_disp(c,d);
}
static bool store_value(Code *c,const IRFunction *fn,IRValue v) {
    int32_t d=value_disp(fn,v);if(!d)return false;
    return byte(c,0x48)&&byte(c,0x89)&&byte(c,0x85)&&emit_disp(c,d);
}
static bool load_local(Code *c,const IRFunction *fn,uint32_t v) {
    int32_t d=local_disp(fn,v);if(!d)return false;
    return byte(c,0x48)&&byte(c,0x8b)&&byte(c,0x85)&&emit_disp(c,d);
}
static bool store_local(Code *c,const IRFunction *fn,uint32_t v) {
    int32_t d=local_disp(fn,v);if(!d)return false;
    return byte(c,0x48)&&byte(c,0x89)&&byte(c,0x85)&&emit_disp(c,d);
}

static bool emit_binary(Code *c,const IRFunction *fn,const IRInst *x) {
    if(!load_value(c,fn,x->a)) return false;
    int32_t d=value_disp(fn,x->b);if(!d)return false;
    switch(x->bin_op) {
    case BIN_ADD: if(!byte(c,0x48)||!byte(c,0x03)||!byte(c,0x85))return false;break;
    case BIN_SUB: if(!byte(c,0x48)||!byte(c,0x2b)||!byte(c,0x85))return false;break;
    case BIN_MUL: if(!byte(c,0x48)||!byte(c,0x0f)||!byte(c,0xaf)||!byte(c,0x85))return false;break;
    case BIN_DIV: case BIN_MOD:
        if(!byte(c,0x48)||!byte(c,0x99)||!byte(c,0x48)||!byte(c,0xf7)||!byte(c,0xbd))return false;
        if(!emit_disp(c,d))return false;
        if(x->bin_op==BIN_MOD && (!byte(c,0x48)||!byte(c,0x89)||!byte(c,0xd0)))return false;
        return store_value(c,fn,x->result);
    case BIN_AND: if(!byte(c,0x48)||!byte(c,0x23)||!byte(c,0x85))return false;break;
    case BIN_OR: if(!byte(c,0x48)||!byte(c,0x0b)||!byte(c,0x85))return false;break;
    case BIN_EQ: case BIN_NE: case BIN_LT: case BIN_LE: case BIN_GT: case BIN_GE: {
        if(!byte(c,0x48)||!byte(c,0x3b)||!byte(c,0x85)||!emit_disp(c,d))return false;
        unsigned cc=x->bin_op==BIN_EQ?0x94:x->bin_op==BIN_NE?0x95:
                    x->bin_op==BIN_LT?0x9c:x->bin_op==BIN_LE?0x9e:
                    x->bin_op==BIN_GT?0x9f:0x9d;
        if(!byte(c,0x0f)||!byte(c,cc)||!byte(c,0xc0)||
           !byte(c,0x0f)||!byte(c,0xb6)||!byte(c,0xc0))return false;
        return store_value(c,fn,x->result);
    }
    default:return false;
    }
    return emit_disp(c,d)&&store_value(c,fn,x->result);
}

typedef struct { size_t displacement_at; uint32_t target; } BranchPatch;
typedef struct { size_t displacement_at; size_t function; } CallPatch;
typedef struct {
    Code *code;
    const IRModule *module;
    CallPatch *calls;
    size_t call_cap, call_count;
} EmitContext;

static bool add_patch(BranchPatch *patches,size_t cap,size_t *count,size_t at,uint32_t target) {
    if(*count>=cap)return false;
    patches[(*count)++]=(BranchPatch){at,target};return true;
}

static size_t find_function(const IRModule *m,ForgeStr module,ForgeStr name) {
    for(size_t i=0;i<m->function_count;i++)
        if(forge_str_eq(m->functions[i].module,module)&&forge_str_eq(m->functions[i].name,name))return i;
    return SIZE_MAX;
}

static size_t function_param_count(const IRFunction *fn) {
    if(!fn->block_count)return SIZE_MAX;
    size_t n=0;
    for(size_t i=0;i<fn->blocks[0].inst_count;i++)
        if(fn->blocks[0].insts[i].op==IR_PARAM)n++;
    return n;
}

static bool move_rax_to_arg(Code *c,size_t arg) {
    static const unsigned char regs[][3]={{0x48,0x89,0xc7},{0x48,0x89,0xc6},
        {0x48,0x89,0xc2},{0x48,0x89,0xc1},{0x49,0x89,0xc0},{0x49,0x89,0xc1}};
    return arg<sizeof(regs)/sizeof(regs[0])&&bytes(c,regs[arg],3);
}

static bool emit_call(EmitContext *ctx,const IRFunction *caller,const IRInst *x) {
    if(x->op!=IR_CALL||!x->type_known)return false;
    size_t callee=find_function(ctx->module,x->module,x->name);
    if(callee==SIZE_MAX)return false;
    const IRFunction *target=&ctx->module->functions[callee];
    if(target->is_extern||target->module.len||function_param_count(target)!=x->arg_count||x->arg_count>6)return false;
    if(target->return_type.kind!=x->type.kind ||
       ((target->return_type.kind==TY_VOID)!=(x->result==IR_NO_VALUE)))return false;
    for(size_t i=0;i<x->arg_count;i++)
        if(!load_value(ctx->code,caller,x->args[i])||!move_rax_to_arg(ctx->code,i))return false;
    if(!byte(ctx->code,0xe8))return false;
    size_t at=ctx->code->len;
    if(!imm32(ctx->code,0)||ctx->call_count>=ctx->call_cap)return false;
    ctx->calls[ctx->call_count++]=(CallPatch){at,callee};
    if(x->result!=IR_NO_VALUE&&!store_value(ctx->code,caller,x->result))return false;
    return true;
}

static bool emit_block_insts(EmitContext *ctx,const IRFunction *fn,const IRBlock *b) {
    Code *c=ctx->code;
    size_t parameter=0;
    for(size_t i=0;i<b->inst_count;i++) {
        const IRInst *x=&b->insts[i];
        switch(x->op) {
        case IR_CONST_INT:
            if(x->result==IR_NO_VALUE||!byte(c,0x48)||!byte(c,0xb8)||!imm64(c,(uint64_t)x->int_value)||!store_value(c,fn,x->result))return false;
            break;
        case IR_CONST_BOOL:
            if(x->result==IR_NO_VALUE||!byte(c,0x48)||!byte(c,0xb8)||!imm64(c,x->bool_value?1:0)||!store_value(c,fn,x->result))return false;
            break;
        case IR_PARAM: {
            static const unsigned char from_arg[][3]={{0x48,0x89,0xf8},{0x48,0x89,0xf0},
                {0x48,0x89,0xd0},{0x48,0x89,0xc8},{0x4c,0x89,0xc0},{0x4c,0x89,0xc8}};
            if(parameter>=sizeof(from_arg)/sizeof(from_arg[0])||x->result==IR_NO_VALUE||
               !bytes(c,from_arg[parameter],3)||!store_value(c,fn,x->result))return false;
            parameter++;
            break;
        }
        case IR_BINARY:
            if(!emit_binary(c,fn,x))return false;
            break;
        case IR_LOAD_LOCAL:
            if(x->result==IR_NO_VALUE||!load_local(c,fn,x->local)||!store_value(c,fn,x->result))return false;
            break;
        case IR_STORE_LOCAL:
            if(!load_value(c,fn,x->a)||!store_local(c,fn,x->local))return false;
            break;
        case IR_EVAL:
            break;
        case IR_CALL:
            if(!emit_call(ctx,fn,x))return false;
            break;
        case IR_PHI:
            /* Phi copies are emitted on their incoming edges. */
            break;
        default:return false;
        }
    }
    return true;
}

static bool validate_phis(const IRFunction *fn) {
    for(size_t bi=0;bi<fn->block_count;bi++) {
        const IRBlock *merge=&fn->blocks[bi];
        for(size_t ii=0;ii<merge->inst_count;ii++) {
            const IRInst *phi=&merge->insts[ii];
            if(phi->op!=IR_PHI)continue;
            if(phi->local>=fn->block_count||phi->int_value<0||
               (uint64_t)phi->int_value>=fn->block_count)return false;
            const IRBlock *left=&fn->blocks[phi->local];
            const IRBlock *right=&fn->blocks[(size_t)phi->int_value];
            if(left->term!=IR_TERM_JUMP||left->target!=bi||
               right->term!=IR_TERM_JUMP||right->target!=bi)return false;
        }
    }
    return true;
}

static bool emit_phi_edge_copies(Code *c,const IRFunction *fn,uint32_t from,uint32_t to) {
    if(to>=fn->block_count)return false;
    const IRBlock *merge=&fn->blocks[to];
    for(size_t ii=0;ii<merge->inst_count;ii++) {
        const IRInst *phi=&merge->insts[ii];
        if(phi->op!=IR_PHI)continue;
        IRValue incoming;
        if(phi->local==from)incoming=phi->a;
        else if(phi->int_value>=0&&(uint64_t)phi->int_value==from)incoming=phi->b;
        else return false;
        if(!load_value(c,fn,incoming)||!store_value(c,fn,phi->result))return false;
    }
    return true;
}

static bool emit_function(EmitContext *ctx,const IRFunction *fn) {
    Code *c=ctx->code;
    if(fn->is_extern||!fn->block_count||fn->local_count>UINT32_MAX-fn->next_value||!validate_phis(fn))return false;
    if(function_param_count(fn)>6)return false;
    for(size_t i=0;i<fn->local_count;i++)
        if(fn->locals[i].type.kind!=TY_INT&&fn->locals[i].type.kind!=TY_BOOL)return false;
    uint64_t slots=(uint64_t)fn->next_value+fn->local_count;
    uint64_t frame=(slots*8+15)&~UINT64_C(15);
    if(frame>INT32_MAX)return false;
    /* push rbp; mov rbp,rsp; sub rsp, aligned frame size */
    if(!byte(c,0x55)||!byte(c,0x48)||!byte(c,0x89)||!byte(c,0xe5))return false;
    if(frame && (!byte(c,0x48)||!byte(c,0x81)||!byte(c,0xec)||!imm32(c,(uint32_t)frame)))return false;
    size_t offset_bytes=fn->block_count*sizeof(size_t),patch_cap=fn->block_count*2;
    size_t *block_offsets=malloc(offset_bytes);
    BranchPatch *patches=calloc(patch_cap,sizeof(BranchPatch));
    if(!block_offsets||!patches){free(block_offsets);free(patches);return false;}
    size_t patch_count=0;
    bool ok=true;
    for(size_t bi=0;bi<fn->block_count&&ok;bi++) {
        const IRBlock *b=&fn->blocks[bi];
        block_offsets[bi]=c->len;
        ok=emit_block_insts(ctx,fn,b);
        if(!ok)break;
        switch(b->term) {
        case IR_TERM_JUMP: {
            ok=emit_phi_edge_copies(c,fn,(uint32_t)bi,b->target)&&byte(c,0xe9);
            size_t at=c->len;ok=ok&&imm32(c,0)&&add_patch(patches,patch_cap,&patch_count,at,b->target);
            break;
        }
        case IR_TERM_BRANCH: {
            ok=load_value(c,fn,b->value)&&byte(c,0x48)&&byte(c,0x85)&&byte(c,0xc0)&&byte(c,0x0f)&&byte(c,0x85);
            size_t yes_at=c->len;ok=ok&&imm32(c,0)&&add_patch(patches,patch_cap,&patch_count,yes_at,b->target);
            ok=ok&&byte(c,0xe9);size_t no_at=c->len;ok=ok&&imm32(c,0)&&add_patch(patches,patch_cap,&patch_count,no_at,b->target_false);
            break;
        }
        case IR_TERM_RETURN:
            if(b->has_value)ok=load_value(c,fn,b->value);
            else ok=byte(c,0x31)&&byte(c,0xc0);
            ok=ok&&byte(c,0xc9)&&byte(c,0xc3);
            break;
        case IR_TERM_NONE: default: ok=false;break;
        }
    }
    for(size_t i=0;i<patch_count&&ok;i++) {
        BranchPatch p=patches[i];
        if(p.target>=fn->block_count){ok=false;break;}
        int64_t rel=(int64_t)block_offsets[p.target]-(int64_t)(p.displacement_at+4);
        if(rel<INT32_MIN||rel>INT32_MAX){ok=false;break;}
        put32(c->data+p.displacement_at,(uint32_t)(int32_t)rel);
    }
    free(block_offsets);free(patches);return ok;
}

static bool write_all(int fd,const unsigned char *p,size_t n) {
    while(n){ssize_t k=write(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;p+=(size_t)k;n-=(size_t)k;}return true;
}
static bool write_executable(const char *path,const unsigned char *data,size_t len) {
    size_t n=strlen(path);char *tmp=malloc(n+48);if(!tmp)return false;int fd=-1;
    for(unsigned i=0;i<100;i++){snprintf(tmp,n+48,"%s.tmp.%ld.%u",path,(long)getpid(),i);fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0700);if(fd>=0||errno!=EEXIST)break;}
    bool ok=fd>=0&&write_all(fd,data,len)&&fchmod(fd,0755)==0;
    if(fd>=0&&close(fd)!=0)ok=false;
    if(ok&&rename(tmp,path)!=0)ok=false;
    if(!ok&&fd>=0)unlink(tmp);
    free(tmp);return ok;
}

static bool emit_elf(const IRModule *m,const char *path) {
    if(!m->function_count||m->global_count||m->struct_count||m->enum_count) {
        fprintf(stderr,"forge: native ELF prototype requires functions without global data\n");return false;
    }
    size_t main_index=SIZE_MAX,total_insts=0;
    for(size_t i=0;i<m->function_count;i++) {
        const IRFunction *fn=&m->functions[i];
        if(fn->module.len||fn->is_extern||find_function(m,forge_str(""),fn->name)!=i||
           (fn->return_type.kind!=TY_INT&&fn->return_type.kind!=TY_BOOL&&fn->return_type.kind!=TY_VOID)) {
            fprintf(stderr,"forge: native ELF prototype supports only unique, non-extern local functions returning int, bool or void\n");return false;
        }
        if(fn->name.len==4&&!memcmp(fn->name.data,"main",4)) {
            if(main_index!=SIZE_MAX||function_param_count(fn)!=0||!fn->is_native) {
                fprintf(stderr,"forge: native entry must be a unique zero-argument `native main`\n");return false;
            }
            main_index=i;
        }
        for(size_t bi=0;bi<fn->block_count;bi++) {
            if(total_insts>SIZE_MAX-fn->blocks[bi].inst_count)return false;
            total_insts+=fn->blocks[bi].inst_count;
        }
    }
    if(main_index==SIZE_MAX) {
        fprintf(stderr,"forge: native ELF prototype requires `native main`\n");return false;
    }
    if(total_insts==SIZE_MAX)return false;
    size_t *function_offsets=calloc(m->function_count,sizeof(size_t));
    size_t call_cap=total_insts+1;
    CallPatch *calls=calloc(call_cap,sizeof(CallPatch));
    if(!function_offsets||!calls){free(function_offsets);free(calls);return false;}
    Code code={0};
    /* _start calls main(int-return), then exits with its result. */
    static const unsigned char start[]={0x31,0xed,0xe8,0,0,0,0,0x89,0xc7,0xb8,60,0,0,0,0x0f,0x05};
    if(!bytes(&code,start,sizeof(start)))goto unsupported;
    EmitContext ctx={&code,m,calls,call_cap,0};
    calls[ctx.call_count++]=(CallPatch){3,main_index};
    for(size_t i=0;i<m->function_count;i++) {
        function_offsets[i]=code.len;
        if(!emit_function(&ctx,&m->functions[i]))goto unsupported;
    }
    for(size_t i=0;i<ctx.call_count;i++) {
        CallPatch p=calls[i];
        if(p.function>=m->function_count)goto unsupported;
        int64_t rel=(int64_t)function_offsets[p.function]-(int64_t)(p.displacement_at+4);
        if(rel<INT32_MIN||rel>INT32_MAX)goto unsupported;
        put32(code.data+p.displacement_at,(uint32_t)(int32_t)rel);
    }
    if(code.len>SIZE_MAX-ELF_EHDR-ELF_PHDR)goto unsupported;
    size_t image_len=ELF_EHDR+ELF_PHDR+code.len;
    unsigned char *image=calloc(image_len,1);if(!image)goto unsupported;
    unsigned char *e=image;
    e[0]=0x7f;e[1]='E';e[2]='L';e[3]='F';e[4]=2;e[5]=1;e[6]=1;
    put16(e+16,2);put16(e+18,62);put32(e+20,1);put64(e+24,UINT64_C(0x400000)+ELF_EHDR+ELF_PHDR);
    put64(e+32,ELF_EHDR);put16(e+52,ELF_EHDR);put16(e+54,ELF_PHDR);put16(e+56,1);
    unsigned char *ph=image+ELF_EHDR;put32(ph,1);put32(ph+4,5);put64(ph+8,0);
    put64(ph+16,UINT64_C(0x400000));put64(ph+24,UINT64_C(0x400000));put64(ph+32,image_len);put64(ph+40,image_len);put64(ph+48,4096);
    memcpy(image+ELF_EHDR+ELF_PHDR,code.data,code.len);
    bool ok=write_executable(path,image,image_len);
    if(!ok)fprintf(stderr,"forge: cannot write executable '%s': %s\n",path,strerror(errno));
    free(image);free(code.data);free(function_offsets);free(calls);return ok;
unsupported:
    fprintf(stderr,"forge: native ELF prototype supports integer constants, arithmetic, comparisons, locals, branches, loops, direct-edge phi values and local calls with up to six arguments\n");
    free(code.data);free(function_offsets);free(calls);return false;
}

bool forge_native_emit(const IRModule *m,const ForgeTarget *t,const char *path) {
    if(!m||!t||!path)return false;
    if(t->os==FORGE_OS_LINUX&&t->arch==FORGE_ARCH_X86_64&&t->object_format==FORGE_OBJECT_ELF)return emit_elf(m,path);
    fprintf(stderr,"forge: no Forge-native executable backend for %s-%s (%s) yet\n",forge_target_arch_name(t->arch),forge_target_os_name(t->os),forge_target_abi_name(t->abi));return false;
}
#else
bool forge_native_emit(const IRModule *m,const ForgeTarget *t,const char *path) {
    (void)m;(void)t;(void)path;fprintf(stderr,"forge: Forge-native executable emission is implemented only on Linux hosts\n");return false;
}
#endif
