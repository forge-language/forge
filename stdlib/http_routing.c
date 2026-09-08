#include "forge/http.h"
#include "http_internal.h"
#include "forge/tcp.h"
#include "forge/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(FORGE_OS_WINDOWS)
#include <errno.h>
#endif

#if defined(FORGE_OS_LINUX)
#include <sys/epoll.h>
#include <unistd.h>
#endif

typedef struct {
    const char *path;
    const char *body;
    char *cached_resp;
    size_t cached_len;
} http_route_entry_t;

typedef struct {
    http_route_entry_t *routes;
    size_t route_count;
    char *not_found;
    size_t not_found_len;
    int64_t port;
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
    http_route_entry_t *routes =
        (http_route_entry_t *)realloc(g_route_table.routes, (n + 1) * sizeof(http_route_entry_t));
    if (!routes) return;
    g_route_table.routes = routes;
    http_route_entry_t *e = &g_route_table.routes[n];
    e->path = path;
    e->body = body;
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
    g_route_table.not_found = build_http_response("{\"error\":\"not_found\"}", &g_route_table.not_found_len);
}

static const http_route_entry_t *route_lookup(const char *path) {
    for (size_t i = 0; i < g_route_table.route_count; i++) {
        if (strcmp(g_route_table.routes[i].path, path) == 0) {
            return &g_route_table.routes[i];
        }
    }
    return NULL;
}

static int parse_request_path(int client, char *path, size_t path_cap) {
    char buf[1024];
    size_t len = 0;
    int complete = 0;
    if (!path || path_cap < 2) return -1;
    path[0] = '\0';
    while (len + 1 < sizeof(buf)) {
        ssize_t n = fr_sock_recv(client, buf + len, sizeof(buf) - len - 1);
        if (n < 0) return -1;
        if (n == 0) return -1;
        len += (size_t)n;
        buf[len] = '\0';
        if (len >= 4 && buf[len - 4] == '\r' && buf[len - 3] == '\n' &&
            buf[len - 2] == '\r' && buf[len - 1] == '\n') {
            complete = 1;
            break;
        }
    }
    if (!complete) return -1;
    const char *line_end = strstr(buf, "\r\n");
    if (!line_end) return -1;
    char line[256];
    size_t ll = (size_t)(line_end - buf);
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, buf, ll);
    line[ll] = '\0';
    char method[16];
    char parsed_path[512];
    if (sscanf(line, "%15s %511s", method, parsed_path) < 2) return -1;
    if (strlen(parsed_path) >= path_cap) return -1;
    memcpy(path, parsed_path, strlen(parsed_path) + 1);
    return 0;
}

static void serve_routing_client(int client) {
    char path[512];
    if (fr_sock_set_blocking(client) != 0 ||
        fr_sock_set_timeout(client, 5000) != 0) {
        fr_sock_close(client);
        return;
    }
    if (parse_request_path(client, path, sizeof(path)) < 0) {
        fr_sock_close(client);
        return;
    }
    const http_route_entry_t *route = route_lookup(path);
    const char *resp;
    size_t resp_len;
    if (route && route->cached_resp) {
        resp = route->cached_resp;
        resp_len = route->cached_len;
    } else if (g_route_table.not_found) {
        resp = g_route_table.not_found;
        resp_len = g_route_table.not_found_len;
    } else {
        fr_sock_close(client);
        return;
    }
    const char *p = resp;
    size_t left = resp_len;
    while (left > 0) {
        ssize_t n = fr_sock_send(client, p, left);
        if (n <= 0) {
            fr_sock_close(client);
            return;
        }
        p += n;
        left -= (size_t)n;
    }
    fr_sock_close(client);
}

static void accept_routing_clients(int listen_fd) {
    for (int burst = 0; burst < 256; burst++) {
        int client = fr_http_accept_nb(listen_fd);
        if (client < 0) break;
        serve_routing_client(client);
    }
}

#if defined(FORGE_OS_LINUX)
static void http_routing_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    (void)srv;
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) return;
    fr_sock_set_nonblocking(listen_fd);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        close(epfd);
        return;
    }
    struct epoll_event events[128];
    for (;;) {
        int n = epoll_wait(epfd, events, 128, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd != listen_fd) continue;
            accept_routing_clients(listen_fd);
        }
    }
}
#else
static void http_routing_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    (void)srv;
    fr_sock_set_nonblocking(listen_fd);
    for (;;) accept_routing_clients(listen_fd);
}
#endif

void fr_http_serve_routing_mt(int64_t port, int64_t threads) {
    route_table_init_defaults();
    g_route_table.port = port;

    int64_t server = fr_http_listen(port);
    if (server < 0) return;

    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv) return;
    fr_http_spawn_workers(server, srv, threads, http_routing_event_loop);
}
