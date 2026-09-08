#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "optimize.h"
#include "module_loader.h"
#include "driver.h"
#include "symbols.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "forge: cannot open '%s'\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        fprintf(stderr, "forge: cannot determine size of input file '%s' (is it a pipe or special file?)\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) forge_die("out of memory");
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    *out_len = n;
    return buf;
}

static void usage(FILE *stream, const char *prog) {
    fprintf(stream, "Forge %s - AOT native compiler\n", FORGE_VERSION);
    fprintf(stream, "Usage:\n");
    fprintf(stream, "  %s <input.fg> -o <binary>          Compile directly to native executable\n", prog);
    fprintf(stream, "  %s <input.fg> -o <output.c> --emit-c   Emit C source only\n", prog);
    fprintf(stream, "  %s --lib <input.fg> -o <lib.a> --header <lib.h>\n", prog);
    fprintf(stream, "Options:\n");
    fprintf(stream, "  -h, --help         Show this help\n");
    fprintf(stream, "  --version          Show compiler version\n");
    fprintf(stream, "  --emit-c           Emit C instead of a native binary\n");
    fprintf(stream, "  --forge-root PATH  Project root (include/, build/lib)\n");
    fprintf(stream, "  --lib-dir PATH     Directory containing libforge_*.a\n");
    fprintf(stream, "  -I PATH            Extra include directory (also searches for .fg modules)\n");
    fprintf(stream, "  -l NAME            Link libforge_NAME.a (repeatable)\n");
    fprintf(stream, "  -L PATH            Extra library search directory (repeatable)\n");
    fprintf(stream, "  --cc PATH          C compiler for native output (default: CC, clang, gcc, or cc)\n");
    fprintf(stream, "  --check            Parse only; exit 0 on success (for LSP / CI)\n");
    fprintf(stream, "  --symbols-json     Print document symbols as JSON to stdout\n");
    fprintf(stream, "  --keep-temp        Keep intermediate object files\n");
    fprintf(stream, "  --                 End options (for filenames beginning with '-')\n");
    fprintf(stream, "Example:\n  %s examples/hello.fg -o hello\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr, argv[0]);
        return 1;
    }

    bool lib_mode = false;
    bool check_only = false;
    bool symbols_json = false;
    const char *input = NULL;
    const char *output = NULL;
    const char *header = NULL;

    ForgeDriverConfig cfg;
    forge_driver_config_init(&cfg);
    forge_driver_detect_paths(&cfg, argv[0]);

#define FORGE_MAX_CLI_PATHS 32
#define FORGE_STRINGIFY_(x) #x
#define FORGE_STRINGIFY(x) FORGE_STRINGIFY_(x)
    const char *includes[FORGE_MAX_CLI_PATHS];
    const char *link_libs[FORGE_MAX_CLI_PATHS];
    const char *lib_dirs[FORGE_MAX_CLI_PATHS];
    size_t include_count = 0;
    size_t link_lib_count = 0;
    size_t lib_dir_count = 0;
    bool end_options = false;

    for (int i = 1; i < argc; i++) {
        if (end_options || argv[i][0] != '-') {
            if (input) {
                fprintf(stderr, "forge: multiple input files are not supported: '%s' and '%s'\n", input, argv[i]);
                return 1;
            }
            input = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            end_options = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("Forge %s\n", FORGE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--header") == 0 ||
            strcmp(argv[i], "--forge-root") == 0 || strcmp(argv[i], "--lib-dir") == 0 ||
            strcmp(argv[i], "--cc") == 0 || strcmp(argv[i], "-I") == 0 ||
            strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-L") == 0) {
            if (i + 1 >= argc || !argv[i + 1][0] || argv[i + 1][0] == '-') {
                fprintf(stderr, "forge: option '%s' requires a value (use ./ for paths beginning with '-')\n", argv[i]);
                return 1;
            }
        }
        if (strcmp(argv[i], "--lib") == 0) {
            lib_mode = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--header") == 0 && i + 1 < argc) {
            header = argv[++i];
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            cfg.emit_c_only = true;
        } else if (strcmp(argv[i], "--forge-root") == 0 && i + 1 < argc) {
            cfg.forge_root = argv[++i];
            forge_driver_detect_paths(&cfg, argv[0]);
        } else if (strcmp(argv[i], "--lib-dir") == 0 && i + 1 < argc) {
            cfg.lib_dir = argv[++i];
        } else if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc) {
            cfg.cc = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--symbols-json") == 0) {
            symbols_json = true;
        } else if (strcmp(argv[i], "--keep-temp") == 0) {
            cfg.keep_intermediate = true;
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            if (include_count >= FORGE_MAX_CLI_PATHS)
                forge_die("too many -I include directories (max " FORGE_STRINGIFY(FORGE_MAX_CLI_PATHS) ")");
            includes[include_count++] = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            if (link_lib_count >= FORGE_MAX_CLI_PATHS)
                forge_die("too many -l link libraries (max " FORGE_STRINGIFY(FORGE_MAX_CLI_PATHS) ")");
            link_libs[link_lib_count++] = argv[++i];
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            if (lib_dir_count >= FORGE_MAX_CLI_PATHS)
                forge_die("too many -L library directories (max " FORGE_STRINGIFY(FORGE_MAX_CLI_PATHS) ")");
            lib_dirs[lib_dir_count++] = argv[++i];
        } else {
            fprintf(stderr, "forge: unknown option '%s'; use --help for usage\n", argv[i]);
            return 1;
        }
    }

    cfg.extra_includes = includes;
    cfg.extra_include_count = include_count;
    cfg.link_libs = link_libs;
    cfg.link_lib_count = link_lib_count;
    cfg.extra_lib_dirs = lib_dirs;
    cfg.extra_lib_dir_count = lib_dir_count;

    if (!input) {
        usage(stderr, argv[0]);
        return 1;
    }
    if (lib_mode && !header) {
        fprintf(stderr, "forge: library mode requires --header\n");
        return 1;
    }

    size_t len = 0;
    char *src = read_file(input, &len);

    Lexer lx;
    lexer_init(&lx, src, len);
    Program prog = parse_program(&lx);

    ForgeModuleConfig mcfg = {
        .entry_path = input,
        .lib_dir = cfg.lib_dir,
        .include_dirs = includes,
        .include_dir_count = include_count,
    };
    forge_load_modules(&prog, &mcfg);
    optimize_program(&prog);

    if (symbols_json) {
        forge_emit_symbols_json(&prog, stdout);
        program_free(&prog);
        free(src);
        return 0;
    }
    if (check_only) {
        program_free(&prog);
        free(src);
        return 0;
    }

    int rc = 0;
    if (lib_mode) {
        if (!output) {
            fprintf(stderr, "forge: library mode requires -o\n");
            rc = 1;
        } else {
            rc = forge_driver_compile_library(&prog, output, header, &cfg);
        }
    } else {
        rc = forge_driver_compile_program(&prog, output, &cfg);
    }

    program_free(&prog);
    free(src);
    return rc;
}
