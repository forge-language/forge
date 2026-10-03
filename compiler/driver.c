#define _GNU_SOURCE
#include "driver.h"
#include "codegen.h"
#include "forge/platform.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if !defined(FORGE_OS_WINDOWS)
#include <sys/wait.h>
#include <unistd.h>
#define FORGE_HAVE_UNISTD 1
#else
#include <process.h>
#define PATH_MAX MAX_PATH
#endif

typedef struct {
    char **items;
    unsigned char *owned;
    size_t count;
    size_t cap;
} Argv;

static void argv_init(Argv *a) {
    a->items = NULL;
    a->owned = NULL;
    a->count = 0;
    a->cap = 0;
}

static void argv_push_internal(Argv *a, char *s, int owned) {
    if (a->count == a->cap) {
        size_t new_cap = a->cap ? a->cap * 2 : 32;
        char **new_items = (char **)realloc(a->items, new_cap * sizeof(char *));
        unsigned char *new_owned =
            (unsigned char *)realloc(a->owned, new_cap * sizeof(unsigned char));
        if (!new_items || !new_owned) forge_die("out of memory");
        a->items = new_items;
        a->owned = new_owned;
        a->cap = new_cap;
    }
    a->items[a->count] = s;
    a->owned[a->count] = owned ? 1 : 0;
    a->count++;
}

static void argv_push(Argv *a, char *s) {
    argv_push_internal(a, s, 0);
}

static void argv_push_owned(Argv *a, char *s) {
    if (!s) forge_die("out of memory");
    argv_push_internal(a, s, 1);
}

static void argv_free(Argv *a) {
    for (size_t i = 0; i < a->count; i++) {
        if (a->owned[i]) free(a->items[i]);
    }
    free(a->items);
    free(a->owned);
    a->items = NULL;
    a->owned = NULL;
    a->count = a->cap = 0;
}

static void argv_add_opt(Argv *a, int opt_level) {
    char opt[8];
    snprintf(opt, sizeof(opt), "-O%d", opt_level < 0 ? 2 : (opt_level > 3 ? 3 : opt_level));
    argv_push_owned(a, strdup(opt));
}

static void argv_add_lto(Argv *a) {
#if !defined(FORGE_OS_WINDOWS)
    argv_push_owned(a, strdup("-flto"));
#endif
}

static void argv_add_includes(Argv *a, const ForgeDriverConfig *cfg) {
    if (cfg->include_dir) {
        size_t n = strlen(cfg->include_dir) + 3;
        char *inc = (char *)malloc(n);
        snprintf(inc, n, "-I%s", cfg->include_dir);
        argv_push_owned(a, inc);
    }
    for (size_t i = 0; i < cfg->extra_include_count; i++) {
        size_t n = strlen(cfg->extra_includes[i]) + 3;
        char *inc = (char *)malloc(n);
        snprintf(inc, n, "-I%s", cfg->extra_includes[i]);
        argv_push_owned(a, inc);
    }
}

static int exec_argv(Argv *a) {
    a->items = (char **)realloc(a->items, (a->count + 1) * sizeof(char *));
    a->items[a->count] = NULL;
#if defined(FORGE_OS_WINDOWS)
    int rc = _spawnvp(_P_WAIT, a->items[0], a->items);
    return rc < 0 ? 1 : rc;
#else
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "forge: fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execvp(a->items[0], a->items);
        fprintf(stderr, "forge: failed to run '%s': %s\n", a->items[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        fprintf(stderr, "forge: waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    return 0;
#endif
}

static int compile_c_source(Program *prog, const char *obj_path, const ForgeDriverConfig *cfg,
                            void (*emit)(Program *, FILE *, const char *)) {
    char cpath[PATH_MAX];
    if (fr_make_temp_path(cpath, sizeof(cpath), "forge", ".c") != 0) {
        fprintf(stderr, "forge: cannot create temp file\n");
        return 1;
    }
    FILE *out = fopen(cpath, "w");
    if (!out) {
        remove(cpath);
        return 1;
    }
    emit(prog, out, "forge_runtime.h");
    if (fclose(out) != 0) { remove(cpath); return 1; }

    Argv args;
    argv_init(&args);
    argv_push(&args, (char *)cfg->cc);
    argv_push(&args, "-std=c11");
    argv_add_opt(&args, cfg->opt_level > 0 ? cfg->opt_level : 3);
    if (cfg->opt_level > 0 && getenv("FORGE_ENABLE_LTO")) argv_add_lto(&args);
    argv_add_includes(&args, cfg);
    argv_push(&args, "-c");
    argv_push(&args, cpath);
    argv_push(&args, "-o");
    argv_push(&args, (char *)obj_path);
    int rc = exec_argv(&args);
    argv_free(&args);
    if (!cfg->keep_intermediate) remove(cpath);
    if (rc != 0) fprintf(stderr, "forge: native compilation failed\n");
    return rc;
}

static void emit_program(Program *prog, FILE *out, const char *runtime_include) {
    codegen_emit(prog, out, runtime_include);
}

static void argv_add_link_extras(Argv *a, const ForgeDriverConfig *cfg) {
    if (!cfg->lib_dir) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/forge.link", cfg->lib_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
        if (!p[0] || p[0] == '#') continue;
        char *save = NULL;
        for (char *tok = strtok_r(p, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
            argv_push_owned(a, strdup(tok));
    }
    fclose(f);
}

static int link_object(const char *obj_path, const char *output_path, const ForgeDriverConfig *cfg) {
    Argv args;
    argv_init(&args);
    argv_push(&args, (char *)cfg->cc);
    argv_push(&args, (char *)obj_path);
    argv_push(&args, "-o");
    argv_push(&args, (char *)output_path);
    if (cfg->lib_dir) {
        size_t n = strlen(cfg->lib_dir) + 3;
        char *libdir = (char *)malloc(n);
        snprintf(libdir, n, "-L%s", cfg->lib_dir);
        argv_push_owned(&args, libdir);
    }
    for (size_t i = 0; i < cfg->extra_lib_dir_count; i++) {
        size_t n = strlen(cfg->extra_lib_dirs[i]) + 3;
        char *libdir = (char *)malloc(n);
        if (!libdir) forge_die("out of memory");
        snprintf(libdir, n, "-L%s", cfg->extra_lib_dirs[i]);
        argv_push_owned(&args, libdir);
    }
    for (size_t i = 0; i < cfg->link_lib_count; i++) {
        size_t n = strlen(cfg->link_libs[i]) + 3;
        char *lib = (char *)malloc(n);
        snprintf(lib, n, "-l%s", cfg->link_libs[i]);
        argv_push_owned(&args, lib);
    }
    argv_push(&args, "-lforge_runtime");
    argv_push(&args, "-lforge_std");
    /* threading in forge_std calls back into runtime; repeat the archive pair
     * for linkers that resolve static archives in one left-to-right pass. */
    argv_push(&args, "-lforge_runtime");
    argv_push(&args, "-lforge_std");
    argv_add_link_extras(&args, cfg);
#if defined(FORGE_OS_WINDOWS)
    argv_push(&args, "-lws2_32");
#else
    argv_push(&args, "-pthread");
#endif
    argv_push(&args, "-lm");
    int rc = exec_argv(&args);
    argv_free(&args);
    return rc;
}

#if defined(FORGE_HAVE_UNISTD)
static bool command_in_path(const char *cmd) {
    const char *path = getenv("PATH");
    if (!path || !cmd || !cmd[0]) return false;

    char *path_copy = strdup(path);
    if (!path_copy) return false;

    bool found = false;
    char *save = NULL;
    for (char *dir = strtok_r(path_copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        size_t len = strlen(dir) + strlen(cmd) + 2;
        char *full = (char *)malloc(len);
        if (!full) continue;
        snprintf(full, len, "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) found = true;
        free(full);
        if (found) break;
    }
    free(path_copy);
    return found;
}
#endif

static const char *detect_default_cc(void) {
    const char *cc = getenv("CC");
    if (cc && cc[0]) return cc;

#if defined(FORGE_HAVE_UNISTD)
    static const char *candidates[] = {"clang", "gcc", "cc", NULL};
    for (size_t i = 0; candidates[i]; i++) {
        if (command_in_path(candidates[i])) return candidates[i];
    }
#endif
    return "cc";
}

void forge_driver_config_init(ForgeDriverConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cc = detect_default_cc();
    cfg->opt_level = 3;
}

static int join_path(char *out, size_t cap, const char *root, const char *suffix) {
    size_t a = strlen(root), b = strlen(suffix);
    if (a >= cap || b >= cap - a) return 0;
    memcpy(out, root, a);
    memcpy(out + a, suffix, b + 1);
    return 1;
}

static int executable_path(const char *argv0, char *out, size_t cap) {
#if defined(FORGE_OS_WINDOWS)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return 0;
    fr_path_normalize(out);
    return 1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n > 0 && (size_t)n < cap - 1) { out[n] = '\0'; return 1; }
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    char unresolved[PATH_MAX];
    if (_NSGetExecutablePath(unresolved, &size) == 0 && realpath(unresolved, out)) return 1;
#endif
#if defined(FORGE_OS_WINDOWS)
    (void)argv0;
    return 0;
#else
    return argv0 && realpath(argv0, out) != NULL;
#endif
}

void forge_driver_detect_paths(ForgeDriverConfig *cfg, const char *argv0) {
    static char rootbuf[PATH_MAX];
    static char libbuf[PATH_MAX];
    static char incbuf[PATH_MAX];

    if (!cfg->forge_root) {
        const char *root = getenv("FORGE_ROOT");
        if (root && root[0]) cfg->forge_root = root;
    }
    if (!cfg->forge_root) {
        char resolved[PATH_MAX];
        if (executable_path(argv0, resolved, sizeof(resolved))) {
            char *slash = strrchr(resolved, '/');
            if (slash) *slash = '\0';
            for (int parent = 0; parent < 2; parent++) {
                slash = strrchr(resolved, '/');
                if (!slash) break;
                *slash = '\0';
                char header[PATH_MAX];
                if (join_path(header, sizeof(header), resolved, "/include/forge_runtime.h") &&
                    fr_path_exists(header)) {
                    memcpy(rootbuf, resolved, strlen(resolved) + 1);
                    cfg->forge_root = rootbuf;
                    break;
                }
            }
        }
    }
    if (!cfg->forge_root) cfg->forge_root = ".";

    if (!cfg->lib_dir) {
        if (join_path(libbuf, sizeof(libbuf), cfg->forge_root, "/build/lib") &&
            fr_path_exists(libbuf)) cfg->lib_dir = libbuf;
        else if (join_path(libbuf, sizeof(libbuf), cfg->forge_root, "/lib") &&
                 fr_path_exists(libbuf)) cfg->lib_dir = libbuf;
    }
    if (!cfg->include_dir) {
        if (join_path(incbuf, sizeof(incbuf), cfg->forge_root, "/include") &&
            fr_path_exists(incbuf)) cfg->include_dir = incbuf;
    }
}

static bool output_is_c(const char *path) {
    if (!path) return false;
    size_t n = strlen(path);
    return n > 2 && strcmp(path + n - 2, ".c") == 0;
}

int forge_driver_compile_program(Program *prog, const char *output_path, const ForgeDriverConfig *cfg) {
    if (cfg->emit_c_only || output_is_c(output_path)) {
        FILE *out = stdout;
        if (output_path) {
            out = fopen(output_path, "w");
            if (!out) {
                fprintf(stderr, "forge: cannot write '%s'\n", output_path);
                return 1;
            }
        }
        codegen_emit(prog, out, "forge_runtime.h");
        return output_path ? (fclose(out) != 0) : (fflush(out) != 0);
    }

    if (!output_path) {
        fprintf(stderr, "forge: native output requires -o <binary>\n");
        return 1;
    }
    if (!cfg->lib_dir) {
        fprintf(stderr, "forge: cannot find runtime libraries; set FORGE_ROOT or pass --lib-dir\n");
        return 1;
    }

    char obj_path[PATH_MAX];
    if (fr_make_temp_path(obj_path, sizeof(obj_path), "forge", ".o") != 0) return 1;

    if (compile_c_source(prog, obj_path, cfg, emit_program) != 0) {
        remove(obj_path);
        return 1;
    }
    int rc = link_object(obj_path, output_path, cfg);
    if (!cfg->keep_intermediate) remove(obj_path);
    return rc;
}

static void emit_library_c(Program *prog, FILE *out, const char *runtime_include) {
    codegen_emit_library(prog, out, NULL, runtime_include);
}

int forge_driver_compile_library(Program *prog, const char *output_a, const char *output_h,
                                 const ForgeDriverConfig *cfg) {
    if (cfg->emit_c_only || (output_a && output_is_c(output_a))) {
        FILE *out_c = fopen(output_a, "w");
        FILE *out_h = fopen(output_h, "w");
        if (!out_c || !out_h) {
            if (out_c) fclose(out_c);
            if (out_h) fclose(out_h);
            fprintf(stderr, "forge: cannot write library output\n");
            return 1;
        }
        codegen_emit_library(prog, out_c, out_h, "forge_runtime.h");
        int c_error = fclose(out_c) != 0;
        int h_error = fclose(out_h) != 0;
        return c_error || h_error;
    }

    if (!output_a || !output_h) {
        fprintf(stderr, "forge: library mode requires -o <archive.a> and --header <file.h>\n");
        return 1;
    }

    FILE *out_h = fopen(output_h, "w");
    if (!out_h) return 1;
    codegen_emit_library(prog, NULL, out_h, "forge_runtime.h");
    if (fclose(out_h) != 0) return 1;

    char obj_path[PATH_MAX];
    if (fr_make_temp_path(obj_path, sizeof(obj_path), "forge-lib", ".o") != 0) return 1;

    if (compile_c_source(prog, obj_path, cfg, emit_library_c) != 0) {
        remove(obj_path);
        return 1;
    }

    Argv args;
    argv_init(&args);
    argv_push(&args, "ar");
    argv_push(&args, "rcs");
    argv_push(&args, (char *)output_a);
    argv_push(&args, obj_path);
    int rc = exec_argv(&args);
    argv_free(&args);
    if (!cfg->keep_intermediate) remove(obj_path);
    return rc;
}
