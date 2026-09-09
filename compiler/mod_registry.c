#include "mod_registry.h"
#include <stdio.h>
#include <stdint.h>

static const ForgeStdFn IO_FNS[] = {
    {"print", "fr_print"},
    {"print_int", "fr_print_int"},
    {"print_str", "fr_print_str"},
    {"eprint", "fr_eprint"},
    {"eprint_int", "fr_io_eprint_int"},
    {"eprintln", "fr_eprintln"},
    {"flush", "fr_io_flush"},
    {"flush_err", "fr_io_flush_err"},
    {"read_line", "fr_io_read_line"},
    {"read_char", "fr_io_read_char"},
    {"read_stdin", "fr_io_read_stdin"},
    {"prompt", "fr_io_prompt"},
    {"write_fd", "fr_io_write_fd"},
    {"read_fd", "fr_io_read_fd"},
    {"stdin_fd", "fr_io_stdin_fd"},
    {"stdout_fd", "fr_io_stdout_fd"},
    {"stderr_fd", "fr_io_stderr_fd"},
};

static const ForgeStdFn STRING_FNS[] = {
    {"str_len", "fr_str_len"},
    {"str_concat", "fr_str_concat"},
    {"str_eq", "fr_str_eq"},
    {"str_sub", "fr_str_sub"},
    {"str_contains", "fr_str_contains"},
    {"str_trim", "fr_str_trim"},
    {"str_char_at", "fr_str_char_at"},
    {"str_append", "fr_str_append"},
    {"str_append_str", "fr_str_append_str"},
    {"str_from_int", "fr_str_from_int"},
    {"str_reset_arena", "fr_str_arena_reset"},
};

static const ForgeStdFn MATH_FNS[] = {
    {"abs_i", "fr_abs_i"},
    {"abs_f", "fr_abs_f"},
    {"min_i", "fr_min_i"},
    {"max_i", "fr_max_i"},
    {"clamp_i", "fr_clamp_i"},
    {"pow_i", "fr_pow_i"},
};

static const ForgeStdFn TIME_FNS[] = {
    {"time_now_ms", "fr_time_now_ms"},
    {"sleep_ms", "fr_sleep_ms"},
};

static const ForgeStdFn FS_FNS[] = {
    {"fs_read", "fr_fs_read"},
    {"fs_write", "fr_fs_write"},
    {"fs_append", "fr_fs_append"},
    {"fs_exists", "fr_fs_exists"},
    {"fs_remove", "fr_fs_remove"},
    {"fs_size", "fr_fs_size"},
    {"fs_is_file", "fr_fs_is_file"},
    {"fs_is_dir", "fr_fs_is_dir"},
    {"fs_mkdir", "fr_fs_mkdir"},
    {"fs_rename", "fr_fs_rename"},
    {"fs_copy", "fr_fs_copy"},
    {"fs_list_dir", "fr_fs_list_dir"},
    {"fs_temp_path", "fr_fs_temp_path"},
};

static const ForgeStdFn OS_FNS[] = {
    {"os_exit", "fr_os_exit"},
    {"os_getenv", "fr_os_getenv"},
    {"os_argc", "fr_os_argc"},
    {"os_argv", "fr_os_argv"},
};

static const ForgeStdFn TCP_FNS[] = {
    {"tcp_listen", "fr_tcp_listen"},
    {"tcp_accept", "fr_tcp_accept"},
    {"tcp_connect", "fr_tcp_connect"},
    {"tcp_send", "fr_tcp_send"},
    {"tcp_recv", "fr_tcp_recv"},
    {"tcp_close", "fr_tcp_close"},
    {"net_init", "fr_net_init"},
};

static const ForgeStdFn UDP_FNS[] = {
    {"udp_bind", "fr_udp_bind"},
    {"udp_send", "fr_udp_send"},
    {"udp_recv", "fr_udp_recv"},
    {"udp_peer", "fr_udp_peer"},
    {"udp_close", "fr_udp_close"},
};

static const ForgeStdFn HTTP_FNS[] = {
    {"http_get", "fr_http_get"},
    {"http_post", "fr_http_post"},
    {"http_listen", "fr_http_listen"},
    {"http_accept", "fr_http_accept"},
    {"http_req_method", "fr_http_req_method"},
    {"http_req_path", "fr_http_req_path"},
    {"http_req_body", "fr_http_req_body"},
    {"http_respond", "fr_http_respond"},
    {"http_close", "fr_http_close"},
    {"http_server_close", "fr_http_server_close"},
    {"http_prepare", "fr_http_prepare"},
    {"http_prepare_sendfile", "fr_http_prepare_sendfile"},
    {"http_serve_prepared", "fr_http_serve_prepared"},
    {"http_serve_forever", "fr_http_serve_forever"},
    {"http_serve_ok", "fr_http_serve_ok"},
    {"http_serve_mt", "fr_http_serve_mt"},
    {"http_serve_hybrid", "fr_http_serve_hybrid"},
    {"http_serve_uring", "fr_http_serve_uring"},
    {"http_listen_tls", "fr_http_listen_tls"},
    {"http_serve_tls_mt", "fr_http_serve_tls_mt"},
    {"http_serve_routing_mt", "fr_http_serve_routing_mt"},
    {"http_has_sendfile", "fr_http_has_sendfile"},
    {"http_has_uring", "fr_http_has_uring"},
    {"http_has_tls", "fr_http_has_tls"},
};

static const ForgeStdFn EVENT_FNS[] = {
    {"event_poll", "fr_event_poll"},
    {"event_add_read", "fr_event_add_read"},
};

static const ForgeStdFn JSON_FNS[] = {
    {"json_get_string", "fr_json_get_string"},
    {"json_get_int", "fr_json_get_int"},
    {"json_stringify_str", "fr_json_stringify_str"},
    {"json_stringify_int", "fr_json_stringify_int"},
    {"json_get_path", "fr_json_get_path_raw"},
    {"json_get_path_str", "fr_json_get_path_string"},
    {"json_get_path_int", "fr_json_get_path_int"},
    {"json_array_len", "fr_json_array_len"},
    {"json_array_at", "fr_json_array_item"},
};

static const ForgeStdFn PROCESS_FNS[] = {
    {"proc_run", "fr_proc_run"},
    {"proc_run_forge", "fr_proc_run_forge"},
    {"proc_output", "fr_proc_output"},
};

static const ForgeStdFn DOCSTORE_FNS[] = {
    {"doc_set", "fr_doc_set"},
    {"doc_get", "fr_doc_get"},
    {"doc_remove", "fr_doc_remove"},
};

static const ForgeStdFn LSPRPC_FNS[] = {
    {"lsp_read", "fr_lsp_read_message"},
    {"lsp_write", "fr_lsp_write_message"},
};

static const ForgeStdFn THREAD_FNS[] = {
    {"thread_cpu_count", "fr_threading_cpu_count"},
    {"thread_yield", "fr_threading_yield"},
    {"thread_pin", "fr_threading_pin"},
    {"thread_self", "fr_threading_self"},
    {"thread_worker_id", "fr_threading_worker_id"},
    {"thread_mutex_create", "fr_threading_mutex_create"},
    {"thread_mutex_lock", "fr_threading_mutex_lock"},
    {"thread_mutex_unlock", "fr_threading_mutex_unlock"},
    {"thread_mutex_destroy", "fr_threading_mutex_destroy"},
    {"thread_join", "fr_threading_join"},
    {"thread_join_all", "fr_threading_join_all"},
};

static const ForgeStdFn GPU_FNS[] = {
    {"gpu_available", "fr_gpu_available"},
    {"gpu_backend", "fr_gpu_backend"},
    {"gpu_device_count", "fr_gpu_device_count"},
    {"gpu_device_name", "fr_gpu_device_name"},
    {"gpu_select_device", "fr_gpu_select_device"},
    {"gpu_alloc", "fr_gpu_alloc"},
    {"gpu_free", "fr_gpu_free"},
    {"gpu_copy", "fr_gpu_copy"},
    {"gpu_sync", "fr_gpu_sync"},
    {"gpu_fill_i32", "fr_gpu_fill_i32"},
    {"gpu_write_i32", "fr_gpu_write_i32"},
    {"gpu_read_i32", "fr_gpu_read_i32"},
    {"gpu_add_i32", "fr_gpu_add_i32"},
    {"gpu_mul_i32", "fr_gpu_mul_i32"},
    {"gpu_run_kernel", "fr_gpu_run_kernel"},
};

#define FORGE_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Deriving both the module name's length and the table's entry count from the
 * literals keeps them from drifting: a hand-written `fn_count` silently hides
 * the tail of a table the moment someone appends to it. */
#define FORGE_MODULE(mod_name, hdr, table)                                     \
    { .name = { (char *)mod_name, sizeof(mod_name) - 1 },                      \
      .header = hdr, .fns = table, .fn_count = FORGE_ARRAY_LEN(table) }

static const ForgeModule MODULES[] = {
    FORGE_MODULE("io", "forge/io.h", IO_FNS),
    FORGE_MODULE("strings", "forge/string.h", STRING_FNS),
    FORGE_MODULE("math", "forge/math.h", MATH_FNS),
    FORGE_MODULE("time", "forge/time.h", TIME_FNS),
    FORGE_MODULE("fs", "forge/fs.h", FS_FNS),
    FORGE_MODULE("os", "forge/os.h", OS_FNS),
    FORGE_MODULE("tcp", "forge/tcp.h", TCP_FNS),
    FORGE_MODULE("udp", "forge/udp.h", UDP_FNS),
    FORGE_MODULE("http", "forge/http.h", HTTP_FNS),
    FORGE_MODULE("event", "forge/event.h", EVENT_FNS),
    FORGE_MODULE("json", "forge/json.h", JSON_FNS),
    FORGE_MODULE("gpu", "forge/gpu.h", GPU_FNS),
    FORGE_MODULE("thread", "forge/threading.h", THREAD_FNS),
    FORGE_MODULE("proc", "forge/process.h", PROCESS_FNS),
    FORGE_MODULE("docstore", "forge/docstore.h", DOCSTORE_FNS),
    FORGE_MODULE("lsprpc", "forge/lsprpc.h", LSPRPC_FNS),
};

/* `a` is a slice and need not be NUL-terminated; `b` is a C string. Avoids the
 * strlen(b) that comparing via forge_str(b) would cost on every probe. */
static bool str_eq_cstr(ForgeStr a, const char *b) {
    return strncmp(a.data, b, a.len) == 0 && b[a.len] == '\0';
}

const ForgeModule *forge_std_module(ForgeStr name) {
    for (size_t i = 0; i < FORGE_ARRAY_LEN(MODULES); i++) {
        if (forge_str_eq(MODULES[i].name, name)) return &MODULES[i];
    }
    return NULL;
}

const char *forge_std_c_name(ForgeStr fr_name, ForgeStr *imports, size_t import_count) {
    for (size_t i = 0; i < import_count; i++) {
        const ForgeModule *mod = forge_std_module(imports[i]);
        if (!mod) continue;
        for (size_t j = 0; j < mod->fn_count; j++) {
            if (str_eq_cstr(fr_name, mod->fns[j].fr_name)) return mod->fns[j].c_name;
        }
    }
    return NULL;
}

const char *forge_std_header(ForgeStr module) {
    const ForgeModule *mod = forge_std_module(module);
    return mod ? mod->header : NULL;
}

int forge_import_is_stdlib(ForgeStr name) {
    return forge_std_module(name) != NULL;
}

/* FNV-1a over the mangled-name components. Used to keep long symbols unique
 * when the caller's buffer cannot hold the whole mangled form: without it two
 * names sharing a long prefix truncate to the very same C identifier, and a
 * call silently binds to the wrong function. */
static uint64_t mangle_hash_bytes(uint64_t h, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Hash suffix: '_' + 16 hex digits + NUL. */
#define FORGE_MANGLE_SUFFIX_LEN 18

static void forge_mangle_into(char *out, size_t cap, const char *prefix,
                              ForgeStr a, ForgeStr b) {
    if (!out || cap == 0) return;
    int n = snprintf(out, cap, "%s%.*s_%.*s", prefix,
                     (int)a.len, a.data, (int)b.len, b.data);
    if (n < 0) forge_die("symbol mangling failed");
    if ((size_t)n < cap) return;

    /* snprintf truncated. Replace the tail with a hash of the full name so
     * distinct sources always produce distinct symbols. */
    if (cap < FORGE_MANGLE_SUFFIX_LEN + 4)
        forge_die("symbol buffer too small for mangled name");
    uint64_t h = 1469598103934665603ULL;
    h = mangle_hash_bytes(h, prefix, strlen(prefix));
    h = mangle_hash_bytes(h, a.data, a.len);
    h = mangle_hash_bytes(h, "_", 1);
    h = mangle_hash_bytes(h, b.data, b.len);
    snprintf(out + cap - FORGE_MANGLE_SUFFIX_LEN, FORGE_MANGLE_SUFFIX_LEN,
             "_%016llx", (unsigned long long)h);
}

void forge_lib_mangle(char *out, size_t cap, ForgeStr lib, ForgeStr fn) {
    forge_mangle_into(out, cap, "frlib_", lib, fn);
}

void forge_mod_mangle(char *out, size_t cap, ForgeStr mod, ForgeStr fn) {
    forge_mangle_into(out, cap, "frmod_", mod, fn);
}

int forge_import_is_file_module(Program *prog, ForgeStr name) {
    for (size_t i = 0; i < prog->module_count; i++) {
        if (forge_str_eq(prog->modules[i].name, name)) return 1;
    }
    return 0;
}
