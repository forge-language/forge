#include "mod_registry.h"
#include <stdio.h>
#include <stdint.h>

static const ForgeStdFn IO_FNS[] = {
    {"print", "fr_print", "vs"},
    {"print_int", "fr_print_int", "vi"},
    {"print_str", "fr_print_str", "vs"},
    {"eprint", "fr_eprint", "vs"},
    {"eprint_int", "fr_io_eprint_int", "vi"},
    {"eprintln", "fr_eprintln", "vs"},
    {"flush", "fr_io_flush", "v"},
    {"flush_err", "fr_io_flush_err", "v"},
    {"read_line", "fr_io_read_line", "s"},
    {"read_char", "fr_io_read_char", "i"},
    {"read_stdin", "fr_io_read_stdin", "s"},
    {"prompt", "fr_io_prompt", "ss"},
    {"write_fd", "fr_io_write_fd", "iis"},
    {"read_fd", "fr_io_read_fd", "sii"},
    {"stdin_fd", "fr_io_stdin_fd", "i"},
    {"stdout_fd", "fr_io_stdout_fd", "i"},
    {"stderr_fd", "fr_io_stderr_fd", "i"},
};

static const ForgeStdFn STRING_FNS[] = {
    {"str_len", "fr_str_len", "is"},
    {"str_concat", "fr_str_concat", "sss"},
    {"str_eq", "fr_str_eq", "iss"},
    {"str_sub", "fr_str_sub", "ssii"},
    {"str_contains", "fr_str_contains", "iss"},
    {"str_trim", "fr_str_trim", "ss"},
    {"str_char_at", "fr_str_char_at", "isi"},
    {"str_append", "fr_str_append", "ssi"},
    {"str_append_str", "fr_str_append_str", "sss"},
    {"str_from_int", "fr_str_from_int", "si"},
    {"str_reset_arena", "fr_str_arena_reset", "v"},
    {"str_view", "fr_str_view", "is"},
    {"str_view_len", "fr_str_view_len", "ii"},
    {"str_view_at", "fr_str_view_at", "iii"},
    {"str_view_sub", "fr_str_view_sub", "siii"},
    {"str_view_matches", "fr_str_view_matches", "iiis"},
    {"str_view_append_byte", "fr_str_view_append_byte", "iii"},
    {"str_builder", "fr_str_builder", "i"},
    {"str_builder_with_capacity", "fr_str_builder_with_capacity", "ii"},
    {"str_builder_append", "fr_str_builder_append", "iis"},
    {"str_builder_append_view", "fr_str_builder_append_view", "iiiii"},
    {"str_builder_char", "fr_str_builder_char", "iii"},
    {"str_builder_finish", "fr_str_builder_finish", "si"},
    {"str_builder_finish_take", "fr_str_builder_finish_take", "si"},

};

static const ForgeStdFn MATH_FNS[] = {
    {"abs_i", "fr_abs_i", "ii"},
    {"abs_f", "fr_abs_f", "ff"},
    {"min_i", "fr_min_i", "iii"},
    {"max_i", "fr_max_i", "iii"},
    {"clamp_i", "fr_clamp_i", "iiii"},
    {"pow_i", "fr_pow_i", "iii"},
};

static const ForgeStdFn TIME_FNS[] = {
    {"time_now_ms", "fr_time_now_ms", "i"},
    {"time_monotonic_ms", "fr_time_monotonic_ms", "i"},
    {"sleep_ms", "fr_sleep_ms", "vi"},
};

static const ForgeStdFn FS_FNS[] = {
    {"fs_read", "fr_fs_read", "ss"},
    {"fs_write", "fr_fs_write", "iss"},
    {"fs_append", "fr_fs_append", "iss"},
    {"fs_exists", "fr_fs_exists", "is"},
    {"fs_remove", "fr_fs_remove", "is"},
    {"fs_size", "fr_fs_size", "is"},
    {"fs_is_file", "fr_fs_is_file", "is"},
    {"fs_is_dir", "fr_fs_is_dir", "is"},
    {"fs_mkdir", "fr_fs_mkdir", "is"},
    {"fs_rename", "fr_fs_rename", "iss"},
    {"fs_copy", "fr_fs_copy", "iss"},
    {"fs_list_dir", "fr_fs_list_dir", "ss"},
    {"fs_temp_path", "fr_fs_temp_path", "sss"},
};

static const ForgeStdFn OS_FNS[] = {
    {"os_exit", "fr_os_exit", "vi"},
    {"os_getenv", "fr_os_getenv", "ss"},
    {"os_argc", "fr_os_argc", "i"},
    {"os_argv", "fr_os_argv", "si"},
    {"os_command", "fr_os_command", "is"},
    {"os_command_arg", "fr_os_command_arg", "iis"},
    {"os_command_run", "fr_os_command_run", "ii"},
    {"os_command_free", "fr_os_command_free", "vi"},
    {"os_temp_file", "fr_os_temp_file", "s"},
    {"os_executable_path", "fr_os_executable_path", "s"},
    {"os_same_file", "fr_os_same_file", "iss"},
};

static const ForgeStdFn TCP_FNS[] = {
    {"tcp_listen", "fr_tcp_listen", "ii"},
    {"tcp_accept", "fr_tcp_accept", "ii"},
    {"tcp_connect", "fr_tcp_connect", "isi"},
    {"tcp_send", "fr_tcp_send", "iis"},
    {"tcp_recv", "fr_tcp_recv", "si"},
    {"tcp_close", "fr_tcp_close", "vi"},
    {"net_init", "fr_net_init", "v"},
};

static const ForgeStdFn UDP_FNS[] = {
    {"udp_bind", "fr_udp_bind", "ii"},
    {"udp_send", "fr_udp_send", "iisis"},
    {"udp_recv", "fr_udp_recv", "si"},
    {"udp_peer", "fr_udp_peer", "si"},
    {"udp_close", "fr_udp_close", "vi"},
};

static const ForgeStdFn HTTP_FNS[] = {
    {"http_get", "fr_http_get", "ss"},
    {"http_post", "fr_http_post", "sss"},
    {"http_listen", "fr_http_listen", "ii"},
    {"http_accept", "fr_http_accept", "ii"},
    {"http_req_method", "fr_http_req_method", "si"},
    {"http_req_path", "fr_http_req_path", "si"},
    {"http_req_body", "fr_http_req_body", "si"},
    {"http_respond", "fr_http_respond", "viis"},
    {"http_close", "fr_http_close", "vi"},
    {"http_server_close", "fr_http_server_close", "vi"},
    {"http_prepare", "fr_http_prepare", "vis"},
    {"http_prepare_sendfile", "fr_http_prepare_sendfile", "vis"},
    {"http_serve_prepared", "fr_http_serve_prepared", "vi"},
    {"http_serve_forever", "fr_http_serve_forever", "vi"},
    {"http_serve_ok", "fr_http_serve_ok", "vis"},
    {"http_serve_mt", "fr_http_serve_mt", "vii"},
    {"http_serve_hybrid", "fr_http_serve_hybrid", "vii"},
    {"http_serve_uring", "fr_http_serve_uring", "vii"},
    {"http_listen_tls", "fr_http_listen_tls", "iiss"},
    {"http_serve_tls_mt", "fr_http_serve_tls_mt", "vii"},
    {"http_serve_routing_mt", "fr_http_serve_routing_mt", "vii"},
    {"http_has_sendfile", "fr_http_has_sendfile", "i"},
    {"http_has_uring", "fr_http_has_uring", "i"},
    {"http_has_tls", "fr_http_has_tls", "i"},
};

static const ForgeStdFn EVENT_FNS[] = {
    {"event_poll", "fr_event_poll", "ipi"},
    {"event_add_read", "fr_event_add_read", "ipi"},
};

static const ForgeStdFn JSON_FNS[] = {
    {"json_get_string", "fr_json_get_string", "sss"},
    {"json_get_int", "fr_json_get_int", "iss"},
    {"json_stringify_str", "fr_json_stringify_str", "sss"},
    {"json_stringify_int", "fr_json_stringify_int", "ssi"},
    {"json_get_path", "fr_json_get_path_raw", "sss"},
    {"json_get_path_str", "fr_json_get_path_string", "sss"},
    {"json_get_path_int", "fr_json_get_path_int", "iss"},
    {"json_array_len", "fr_json_array_len", "is"},
    {"json_array_at", "fr_json_array_item", "ssi"},
};

static const ForgeStdFn PROCESS_FNS[] = {
    {"proc_run", "fr_proc_run", "is"},
    {"proc_run_forge", "fr_proc_run_forge", "issssss"},
    {"proc_output", "fr_proc_output", "s"},
    {"proc_start_forge", "fr_proc_start_forge", "issssss"},
    {"proc_watch_termination", "fr_proc_watch_termination", "i"},
    {"proc_termination_requested", "fr_proc_termination_requested", "i"},
    {"proc_poll", "fr_proc_poll", "ii"},
    {"proc_cancel", "fr_proc_cancel", "ii"},
    {"proc_status", "fr_proc_status", "ii"},
    {"proc_signal", "fr_proc_signal", "ii"},
    {"proc_error_kind", "fr_proc_error_kind", "ii"},
    {"proc_stdout", "fr_proc_stdout", "si"},
    {"proc_stderr", "fr_proc_stderr", "si"},
    {"proc_release", "fr_proc_release", "ii"},
};

static const ForgeStdFn DOCSTORE_FNS[] = {
    {"doc_set", "fr_doc_set", "iss"},
    {"doc_get", "fr_doc_get", "ss"},
    {"doc_remove", "fr_doc_remove", "is"},
};

static const ForgeStdFn LSPRPC_FNS[] = {
    {"lsp_read", "fr_lsp_read_message", "s"},
    {"lsp_poll", "fr_lsp_poll", "ii"},
    {"lsp_message", "fr_lsp_message", "s"},
    {"lsp_write", "fr_lsp_write_message", "vs"},
};

static const ForgeStdFn THREAD_FNS[] = {
    {"thread_cpu_count", "fr_threading_cpu_count", "i"},
    {"thread_yield", "fr_threading_yield", "v"},
    {"thread_pin", "fr_threading_pin", "vi"},
    {"thread_self", "fr_threading_self", "i"},
    {"thread_worker_id", "fr_threading_worker_id", "i"},
    {"thread_mutex_create", "fr_threading_mutex_create", "i"},
    {"thread_mutex_lock", "fr_threading_mutex_lock", "vi"},
    {"thread_mutex_unlock", "fr_threading_mutex_unlock", "vi"},
    {"thread_mutex_destroy", "fr_threading_mutex_destroy", "vi"},
    {"thread_join", "fr_threading_join", "ii"},
    {"thread_join_all", "fr_threading_join_all", "v"},
};

static const ForgeStdFn GPU_FNS[] = {
    {"gpu_available", "fr_gpu_available", "i"},
    {"gpu_backend", "fr_gpu_backend", "s"},
    {"gpu_device_count", "fr_gpu_device_count", "i"},
    {"gpu_device_name", "fr_gpu_device_name", "si"},
    {"gpu_select_device", "fr_gpu_select_device", "ii"},
    {"gpu_alloc", "fr_gpu_alloc", "ii"},
    {"gpu_free", "fr_gpu_free", "vi"},
    {"gpu_copy", "fr_gpu_copy", "iiii"},
    {"gpu_sync", "fr_gpu_sync", "v"},
    {"gpu_fill_i32", "fr_gpu_fill_i32", "iiii"},
    {"gpu_write_i32", "fr_gpu_write_i32", "iiii"},
    {"gpu_read_i32", "fr_gpu_read_i32", "iii"},
    {"gpu_add_i32", "fr_gpu_add_i32", "iiiii"},
    {"gpu_mul_i32", "fr_gpu_mul_i32", "iiiii"},
    {"gpu_run_kernel", "fr_gpu_run_kernel", "issiiii"},
};

static const ForgeModule MODULES[] = {
    { .name = { "io", 2 }, .header = "forge/io.h", .fns = IO_FNS, .fn_count = 17 },
    { .name = { "strings", 7 }, .header = "forge/string.h", .fns = STRING_FNS, .fn_count = sizeof(STRING_FNS) / sizeof(STRING_FNS[0]) },
    { .name = { "math", 4 }, .header = "forge/math.h", .fns = MATH_FNS, .fn_count = 6 },
    { .name = { "time", 4 }, .header = "forge/time.h", .fns = TIME_FNS, .fn_count = sizeof(TIME_FNS) / sizeof(TIME_FNS[0]) },
    { .name = { "fs", 2 }, .header = "forge/fs.h", .fns = FS_FNS, .fn_count = 13 },
    { .name = { "os", 2 }, .header = "forge/os.h", .fns = OS_FNS, .fn_count = 11 },
    { .name = { "tcp", 3 }, .header = "forge/tcp.h", .fns = TCP_FNS, .fn_count = 7 },
    { .name = { "udp", 3 }, .header = "forge/udp.h", .fns = UDP_FNS, .fn_count = 5 },
    { .name = { "http", 4 }, .header = "forge/http.h", .fns = HTTP_FNS, .fn_count = 24 },
    { .name = { "event", 5 }, .header = "forge/event.h", .fns = EVENT_FNS, .fn_count = 2 },
    { .name = { "json", 4 }, .header = "forge/json.h", .fns = JSON_FNS, .fn_count = 9 },
    { .name = { "gpu", 3 }, .header = "forge/gpu.h", .fns = GPU_FNS, .fn_count = 15 },
    { .name = { "thread", 6 }, .header = "forge/threading.h", .fns = THREAD_FNS, .fn_count = 11 },
    { .name = { "proc", 4 }, .header = "forge/process.h", .fns = PROCESS_FNS, .fn_count = sizeof(PROCESS_FNS) / sizeof(PROCESS_FNS[0]) },
    { .name = { "docstore", 8 }, .header = "forge/docstore.h", .fns = DOCSTORE_FNS, .fn_count = 3 },
    { .name = { "lsprpc", 6 }, .header = "forge/lsprpc.h", .fns = LSPRPC_FNS, .fn_count = sizeof(LSPRPC_FNS) / sizeof(LSPRPC_FNS[0]) },
};

const ForgeModule *forge_std_module(ForgeStr name) {
    for (size_t i = 0; i < sizeof(MODULES) / sizeof(MODULES[0]); i++) {
        if (forge_str_eq(MODULES[i].name, name)) return &MODULES[i];
    }
    return NULL;
}

const ForgeStdFn *forge_std_function(ForgeStr fr_name, ForgeStr *imports, size_t import_count) {
    for (size_t i = 0; i < import_count; i++) {
        const ForgeModule *mod = forge_std_module(imports[i]);
        if (!mod) continue;
        for (size_t j = 0; j < mod->fn_count; j++) {
            ForgeStr fn = forge_str(mod->fns[j].fr_name);
            if (forge_str_eq(fn, fr_name)) return &mod->fns[j];
        }
    }
    return NULL;
}

const char *forge_std_c_name(ForgeStr fr_name, ForgeStr *imports, size_t import_count) {
    const ForgeStdFn *fn = forge_std_function(fr_name, imports, import_count);
    return fn ? fn->c_name : NULL;
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
