#include "forge/http.h"
#include "http_internal.h"
#include "forge/tcp.h"
#include "forge/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *path;
    size_t path_len;
    char *cached_resp;
    size_t cached_len;
} http_route_entry_t;

typedef struct {
    http_route_entry_t *routes;
    size_t route_count;
    char *not_found;
    size_t not_found_len;
} http_route_table_t;

static http_route_table_t g_route_table;

static char *build_http_response(const char *body, size_t *out_len) {
    size_t blen = body ? strlen(body) : 0;
    size_t cap = 160 + blen;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap,
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%.*s",
                     blen, (int)blen, body ? body : "");
    if (n <= 0) {
        free(out);
        return NULL;
    }
    *out_len = (size_t)n;
    return out;
}

static void route_table_clear(void) {
    for (size_t i = 0; i < g_route_table.route_count; i++) {
        free(g_route_table.routes[i].cached_resp);
    }
    free(g_route_table.routes);
    free(g_route_table.not_found);
    memset(&g_route_table, 0, sizeof(g_route_table));
}

static void route_table_add(const char *path, const char *body) {
    size_t n = g_route_table.route_count;
    http_route_entry_t *routes = (http_route_entry_t *)realloc(
        g_route_table.routes, (n + 1) * sizeof(http_route_entry_t));
    if (!routes) return;
    g_route_table.routes = routes;
    http_route_entry_t *e = &routes[n];
    e->path = path;
    e->path_len = strlen(path);
    e->cached_resp = build_http_response(body, &e->cached_len);
    g_route_table.route_count = n + 1;
}

static void route_table_init_defaults(void) {
    route_table_clear();
    route_table_add("/", "{\"msg\":\"Hello, World\"}");
    route_table_add("/api/health", "{\"status\":\"ok\"}");
    route_table_add("/api/users", "{\"users\":[{\"id\":1,\"name\":\"alice\"},{\"id\":2,\"name\":\"bob\"}]}");
    route_table_add("/api/users/1", "{\"id\":1,\"name\":\"alice\",\"role\":\"admin\"}");
    route_table_add("/api/posts", "{\"posts\":[{\"id\":10,\"title\":\"Forge\"},{\"id\":11,\"title\":\"Bench\"}]}");
    route_table_add("/api/metrics", "{\"cpu\":0.12,\"mem_mb\":2.4,\"rps\":13000}");
    route_table_add("/api/version", "{\"forge\":\"0.3.0\",\"bench\":\"routing\"}");
    route_table_add("/static/app.js", "// bundled app placeholder");
    route_table_add("/static/style.css", "body{margin:0}");
    g_route_table.not_found =
        build_http_response("{\"error\":\"not_found\"}", &g_route_table.not_found_len);
}

/* Linear scan; the table is small and fixed, and comparing the cached length
 * first rejects almost every non-match without touching the string. */
static const http_route_entry_t *route_lookup(const char *path, size_t path_len) {
    for (size_t i = 0; i < g_route_table.route_count; i++) {
        const http_route_entry_t *e = &g_route_table.routes[i];
        if (e->path_len == path_len && memcmp(e->path, path, path_len) == 0) return e;
    }
    return NULL;
}

/* Read the request head and extract the request-target. Returns its length,
 * or -1 on a malformed/incomplete request. */
static int parse_request_path(int client, char *path, size_t path_cap) {
    char buf[FR_HTTP_HEADER_BUF];
    size_t total_len = 0, header_len = 0;
    if (!path || path_cap < 2) return -1;
    if (fr_http_recv_until_headers(client, buf, sizeof(buf), &total_len, &header_len) < 0) {
        return -1;
    }

    const char *line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;
    char line[256];
    size_t ll = (size_t)(line_end - buf);
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, buf, ll);
    line[ll] = '\0';

    char method[16];
    char parsed[512];
    if (sscanf(line, "%15s %511s", method, parsed) < 2) return -1;
    size_t plen = strlen(parsed);
    if (plen >= path_cap) return -1;
    memcpy(path, parsed, plen + 1);
    return (int)plen;
}

static void serve_routing_client(int client, fr_http_server_state_t *srv) {
    (void)srv; /* routes are global, not per-listener */
    char path[512];
    if (fr_http_conn_setup(client) != 0) {
        fr_sock_close(client);
        return;
    }
    int path_len = parse_request_path(client, path, sizeof(path));
    if (path_len < 0) {
        fr_sock_close(client);
        return;
    }

    const http_route_entry_t *route = route_lookup(path, (size_t)path_len);
    if (route && route->cached_resp) {
        fr_http_send_all(client, route->cached_resp, route->cached_len);
    } else if (g_route_table.not_found) {
        fr_http_send_all(client, g_route_table.not_found, g_route_table.not_found_len);
    }
    fr_sock_close(client);
}

static void http_routing_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    fr_http_event_loop(listen_fd, srv, serve_routing_client);
}

void fr_http_serve_routing_mt(int64_t port, int64_t threads) {
    route_table_init_defaults();

    int64_t server = fr_http_listen(port);
    if (server < 0) return;

    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv) return;
    fr_http_spawn_workers(server, srv, (int)threads, http_routing_event_loop);
}
