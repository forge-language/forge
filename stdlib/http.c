#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "forge/http.h"
#include "http_internal.h"
#include "forge/tcp.h"
#include "forge/platform.h"
#include "forge/thread.h"
#include "forge_runtime.h"
#include "forge/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(FORGE_OS_WINDOWS)
#include <errno.h>
#include <sys/socket.h>
#endif

#if defined(FORGE_OS_LINUX)
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <unistd.h>
#elif defined(FORGE_OS_MACOS)
#include <sys/event.h>
#include <unistd.h>
#endif

#define HTTP_ACCEPT_BURST 256
#define HTTP_NATIVE_QUEUE_HIGH_WATER 2048
#define FR_HTTP_MAX_REQS 4096
#define FR_HTTP_MAX_SERVERS 256
#define FR_HTTP_MAX_BODY (8 * 1024 * 1024)
/* Idle-read timeout applied to accepted request sockets (SO_RCVTIMEO), matching
 * the convention used by serve_client/http_routing/http_tls/http_uring: a client
 * that stops sending mid-request (Slowloris) gets its recv() unblocked and the
 * connection torn down instead of tying up a coroutine/worker forever. */
#define HTTP_IO_TIMEOUT_MS 5000

typedef struct {
    int64_t sock;
    char method[16];
    char path[512];
    char *body;
    /* Per-request arena backing `body` (and any future per-request scratch
     * data). Scoped to exactly this request's lifetime (created lazily in
     * parse_http_request, destroyed in fr_http_close) rather than sharing a
     * thread-local arena, since concurrent requests can time-share one OS
     * thread under the M:N coroutine scheduler -- a shared per-thread arena
     * reset on close would clobber another still-live request's body. */
    fr_arena_t *arena;
} fr_http_req_t;

static fr_http_req_t g_reqs[FR_HTTP_MAX_REQS];
static fr_http_server_state_t g_servers[FR_HTTP_MAX_SERVERS];

fr_http_server_state_t *fr_http_state(int64_t server) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return NULL;
    return &g_servers[server];
}

/* Find the first "\r\n\r\n" occurring anywhere in buf[0..len). Unlike checking
 * only the tail of the most recently read chunk, this correctly detects the
 * terminator even when a single recv() returned the end of the headers AND
 * some/all of the body in the same call (the common case for real clients). */
static const char *find_header_terminator(const char *buf, size_t len) {
    if (len < 4) return NULL;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

/* Reads into buf until the header terminator is found anywhere in the
 * accumulated data. On success, *out_total_len is the number of bytes read so
 * far (headers plus any body bytes that arrived in the same recv()s) and
 * *out_header_len is the offset of the first byte after "\r\n\r\n" — i.e. where
 * the body (if any already buffered) begins. */
static int recv_until_headers(int fd, char *buf, size_t cap,
                              size_t *out_total_len, size_t *out_header_len) {
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t n = fr_sock_recv(fd, buf + len, cap - len - 1);
        if (n < 0) return -1;
        if (n == 0) break;
        len += (size_t)n;
        buf[len] = '\0';
        const char *term = find_header_terminator(buf, len);
        if (term) {
            *out_total_len = len;
            *out_header_len = (size_t)(term - buf) + 4;
            return 0;
        }
    }
    return -1;
}

static int tcp_send_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = fr_sock_send(fd, p + sent, len - sent);
        if (n < 0) {
#if !defined(FORGE_OS_WINDOWS)
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int fr_http_discard_headers(int client) {
    /* 8KB, matching common server defaults (e.g. nginx's
     * large_client_header_buffers) -- 512 bytes is too small for real
     * browser requests once cookies/User-Agent/Accept-* headers are
     * included, causing them to be truncated/rejected. Stack-allocated to
     * match the convention already used by fr_http_accept's read buffer. */
    char buf[8192];
    size_t len = 0;
    while (len + 1 < sizeof(buf)) {
        ssize_t n = fr_sock_recv(client, buf + len, sizeof(buf) - len - 1);
        if (n < 0) {
#if !defined(FORGE_OS_WINDOWS)
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (n == 0) return -1;
        len += (size_t)n;
        buf[len] = '\0';
        /* Scan the entire accumulated buffer (not just the tail of the
         * latest chunk) so a terminator split across two recv() calls is
         * not missed -- same fix pattern as recv_until_headers. */
        if (find_header_terminator(buf, len)) {
            return 0;
        }
    }
    return -1;
}

int fr_http_send_prepared(int client, fr_http_server_state_t *srv) {
    if (!srv || !srv->cached_resp || srv->cached_len == 0) return -1;
#if defined(FORGE_OS_LINUX)
    if (srv->sendfile_fd >= 0 && srv->sendfile_len > 0) {
        off_t offset = 0;
        int would_block_retries = 0;
        while ((size_t)offset < srv->sendfile_len) {
            ssize_t n = sendfile(client, srv->sendfile_fd, &offset,
                                 srv->sendfile_len - (size_t)offset);
            if (n > 0) {
                would_block_retries = 0;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && fr_sock_would_block(errno)) {
                if (++would_block_retries >= 100000) return -1;
                fr_platform_sleep_us(50);
                continue;
            }
            return -1;
        }
        return 0;
    }
#endif
    return tcp_send_all(client, srv->cached_resp, srv->cached_len);
}

static void parse_url(const char *url, char *host, size_t hcap, int64_t *port, char *path, size_t pcap) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= hcap) hlen = hcap - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoll(colon + 1);
        if (slash) snprintf(path, pcap, "%s", slash);
        else snprintf(path, pcap, "/");
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= hcap) hlen = hcap - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = 80;
        if (slash) snprintf(path, pcap, "%s", slash);
        else snprintf(path, pcap, "/");
    }
}

static char *http_exchange(const char *method, const char *url, const char *body) {
    char host[256], path[512];
    int64_t port = 80;
    parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    int64_t sock = fr_tcp_connect(host, port);
    if (sock < 0) return NULL;

    char header[1024];
    if (body && body[0]) {
        snprintf(header, sizeof(header),
                 "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 method, path, host, strlen(body));
    } else {
        snprintf(header, sizeof(header),
                 "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 method, path, host);
    }
    fr_tcp_send(sock, header);
    if (body && body[0]) fr_tcp_send(sock, body);

    char *resp = fr_tcp_recv(sock);
    fr_tcp_close(sock);
    if (!resp) return NULL;

    char *body_start = strstr(resp, "\r\n\r\n");
    if (!body_start) return resp;
    body_start += 4;
    char *out = (char *)malloc(strlen(body_start) + 1);
    if (!out) { free(resp); return NULL; }
    strcpy(out, body_start);
    free(resp);
    return out;
}

char *fr_http_get(const char *url) {
    return http_exchange("GET", url, NULL);
}

char *fr_http_post(const char *url, const char *body) {
    return http_exchange("POST", url, body ? body : "");
}

int64_t fr_http_listen(int64_t port) {
    int64_t sock = fr_tcp_listen(port);
    if (sock < 0) return -1;
    if (sock >= FR_HTTP_MAX_SERVERS) {
        fr_tcp_close(sock);
        return -1;
    }
    g_servers[sock].listen_sock = sock;
    g_servers[sock].port = port;
    g_servers[sock].cached_resp = NULL;
    g_servers[sock].cached_len = 0;
    g_servers[sock].sendfile_fd = -1;
    g_servers[sock].sendfile_len = 0;
    g_servers[sock].tls_ctx = NULL;
    return sock;
}

static int header_ci_equal(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

/* Case-insensitive search for a "Name:" header within headers[0..header_len),
 * returning a pointer to the value (leading spaces skipped) or NULL. */
static const char *find_header_value(const char *headers, size_t header_len, const char *name) {
    size_t name_len = strlen(name);
    if (header_len < name_len) return NULL;
    for (size_t i = 0; i + name_len <= header_len; i++) {
        if ((i == 0 || headers[i - 1] == '\n') &&
            header_ci_equal(headers + i, name, name_len)) {
            const char *v = headers + i + name_len;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
    }
    return NULL;
}

/* raw[0..total_len) holds everything read so far; raw[0..header_len) is the
 * headers (including the trailing "\r\n\r\n"); raw[header_len..total_len) is
 * whatever body bytes already arrived in the same recv()s as the headers. If
 * Content-Length says there is more body than that, the remainder is read
 * from fd (which already has the idle-read timeout set on it). */
static void parse_http_request(int fd, const char *raw, size_t total_len,
                               size_t header_len, fr_http_req_t *req) {
    req->method[0] = req->path[0] = '\0';
    req->body = NULL;

    const char *line_end = strstr(raw, "\r\n");
    if (!line_end) return;
    char line[1024];
    size_t ll = (size_t)(line_end - raw);
    if (ll >= sizeof(line)) ll = sizeof(line) - 1;
    memcpy(line, raw, ll);
    line[ll] = '\0';

    sscanf(line, "%15s %511s", req->method, req->path);

    long content_length = 0;
    const char *cl = find_header_value(raw, header_len, "Content-Length:");
    if (cl) content_length = atol(cl);
    if (content_length < 0) content_length = 0;

    size_t body_have = total_len - header_len;
    const char *body_start = raw + header_len;

    if (content_length == 0) {
        if (body_have > 0) {
            /* Lazily-created, per-request arena (see fr_http_req_t::arena):
             * scoped to exactly this request, so it can never be reset out
             * from under a different still-live request sharing the same
             * OS thread. */
            req->arena = fr_arena_create(0);
            if (req->arena) {
                char *out = (char *)fr_arena_alloc(req->arena, body_have + 1, 1);
                if (out) {
                    memcpy(out, body_start, body_have);
                    out[body_have] = '\0';
                    req->body = out;
                }
            }
        }
        return;
    }

    req->arena = fr_arena_create(0);
    if (!req->arena) return;
    size_t want = (size_t)content_length;
    if (want > FR_HTTP_MAX_BODY) want = FR_HTTP_MAX_BODY;
    char *body_buf = (char *)fr_arena_alloc(req->arena, want + 1, 1);
    if (!body_buf) return;

    size_t have = body_have < want ? body_have : want;
    memcpy(body_buf, body_start, have);
    size_t got = have;
    while (got < want) {
        ssize_t n = fr_sock_recv(fd, body_buf + got, want - got);
        if (n <= 0) break; /* idle timeout or peer closed early; keep what we have */
        got += (size_t)n;
    }
    body_buf[got] = '\0';
    req->body = body_buf;
}

int64_t fr_http_accept(int64_t server) {
    int64_t client = fr_tcp_accept(server);
    if (client < 0) return -1;
    if (client >= FR_HTTP_MAX_REQS) {
        fr_tcp_close(client);
        return -1;
    }

    if (fr_sock_set_blocking((int)client) != 0 ||
        fr_sock_set_timeout((int)client, HTTP_IO_TIMEOUT_MS) != 0) {
        fr_tcp_close(client);
        return -1;
    }

    char buf[4096];
    size_t total_len = 0, header_len = 0;
    if (recv_until_headers((int)client, buf, sizeof(buf), &total_len, &header_len) < 0) {
        fr_tcp_close(client);
        return -1;
    }

    g_reqs[client].sock = client;
    g_reqs[client].arena = NULL;
    parse_http_request((int)client, buf, total_len, header_len, &g_reqs[client]);
    return client;
}

const char *fr_http_req_method(int64_t req) {
    if (req < 0 || req >= FR_HTTP_MAX_REQS) return "";
    return g_reqs[req].method;
}

const char *fr_http_req_path(int64_t req) {
    if (req < 0 || req >= FR_HTTP_MAX_REQS) return "";
    return g_reqs[req].path;
}

const char *fr_http_req_body(int64_t req) {
    if (req < 0 || req >= FR_HTTP_MAX_REQS || !g_reqs[req].body) return "";
    return g_reqs[req].body;
}

void fr_http_respond(int64_t req, int64_t status, const char *body) {
    if (req < 0 || req >= FR_HTTP_MAX_REQS) return;
    const char *text = "OK";
    if (status == 404) text = "Not Found";
    else if (status == 500) text = "Internal Server Error";

    size_t blen = body ? strlen(body) : 0;
    char out[576];
    int hlen = snprintf(out, sizeof(out),
                        "HTTP/1.1 %lld %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                        (long long)status, text, blen);
    if (hlen < 0 || (size_t)hlen >= sizeof(out)) return;

    if (blen > 0 && (size_t)hlen + blen < sizeof(out)) {
        memcpy(out + hlen, body, blen);
        tcp_send_all((int)g_reqs[req].sock, out, (size_t)hlen + blen);
    } else {
        tcp_send_all((int)g_reqs[req].sock, out, (size_t)hlen);
        if (blen > 0) tcp_send_all((int)g_reqs[req].sock, body, blen);
    }
}

void fr_http_close(int64_t req) {
    if (req < 0 || req >= FR_HTTP_MAX_REQS) return;
    /* Destroy this request's own arena (if one was created) instead of
     * resetting a shared thread-local arena: other requests may be
     * concurrently live on the same OS thread (M:N coroutine scheduler),
     * and resetting a shared arena here would silently corrupt their
     * still-in-use body pointers. */
    if (g_reqs[req].arena) {
        fr_arena_destroy(g_reqs[req].arena);
        g_reqs[req].arena = NULL;
    }
    fr_tcp_close(g_reqs[req].sock);
    g_reqs[req].sock = -1;
    g_reqs[req].body = NULL;
}

void fr_http_server_close(int64_t server) {
    if (server >= 0 && server < FR_HTTP_MAX_SERVERS) {
        free(g_servers[server].cached_resp);
        g_servers[server].cached_resp = NULL;
        g_servers[server].cached_len = 0;
        if (g_servers[server].sendfile_fd >= 0) {
            close(g_servers[server].sendfile_fd);
            g_servers[server].sendfile_fd = -1;
            g_servers[server].sendfile_len = 0;
        }
    }
    fr_tcp_close(server);
}

void fr_http_prepare(int64_t server, const char *body) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    free(g_servers[server].cached_resp);
    g_servers[server].cached_resp = NULL;
    g_servers[server].cached_len = 0;

    size_t blen = body ? strlen(body) : 0;
    size_t cap = 128 + blen;
    char *out = (char *)malloc(cap);
    if (!out) return;
    int n = snprintf(out, cap,
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%.*s",
                     blen, (int)blen, body ? body : "");
    if (n <= 0) { free(out); return; }
    g_servers[server].cached_resp = out;
    g_servers[server].cached_len = (size_t)n;
}

void fr_http_prepare_sendfile(int64_t server, const char *body) {
    fr_http_prepare(server, body);
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    fr_http_server_state_t *srv = &g_servers[server];
    if (!srv->cached_resp || srv->cached_len == 0) return;

#if defined(FORGE_OS_LINUX)
    if (srv->sendfile_fd >= 0) {
        close(srv->sendfile_fd);
        srv->sendfile_fd = -1;
        srv->sendfile_len = 0;
    }
    int fd = (int)memfd_create("forge_http_resp", MFD_CLOEXEC);
    if (fd < 0) return;
    ssize_t wrote = 0;
    while ((size_t)wrote < srv->cached_len) {
        ssize_t n = write(fd, srv->cached_resp + wrote, srv->cached_len - (size_t)wrote);
        if (n <= 0) {
            close(fd);
            return;
        }
        wrote += n;
    }
    srv->sendfile_fd = fd;
    srv->sendfile_len = srv->cached_len;
#else
    (void)body;
#endif
}

void fr_http_serve_prepared(int64_t server) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    fr_http_server_state_t *srv = &g_servers[server];
    if (!srv->cached_resp || srv->cached_len == 0) return;

    int client = (int)fr_tcp_accept(server);
    if (client < 0) return;

    char discard[4096];
    fr_sock_recv(client, discard, sizeof(discard));
    fr_http_send_prepared(client, srv);
    fr_sock_close(client);
}

int fr_http_accept_nb(int listen_fd) {
#if defined(FORGE_OS_LINUX)
    return fr_sock_accept_nb(listen_fd);
#else
    fr_socket_t client = accept((fr_socket_t)listen_fd, NULL, NULL);
    if (client == FR_SOCK_INVALID) return -1;
    fr_sock_set_nonblocking((int)client);
    return (int)client;
#endif
}

static void serve_client(fr_http_server_state_t *srv, int client) {
    if (fr_sock_set_blocking(client) != 0 ||
        fr_sock_set_timeout(client, 5000) != 0) {
        fr_sock_close(client);
        return;
    }
    if (fr_http_discard_headers(client) < 0) {
        fr_sock_close(client);
        return;
    }
    if (fr_http_send_prepared(client, srv) < 0) {
        fr_sock_close(client);
        return;
    }
    fr_sock_close(client);
}

typedef struct {
    int client;
    fr_http_server_state_t *srv;
} http_serve_ctx_t;

static fr_scheduler_t *g_http_sched;

static void http_serve_task(void *arg) {
    http_serve_ctx_t *ctx = (http_serve_ctx_t *)arg;
    if (!ctx) return;
    serve_client(ctx->srv, ctx->client);
    free(ctx);
}

static int http_submit_client(fr_http_server_state_t *srv, int client) {
    if (!g_http_sched) {
        serve_client(srv, client);
        return 0;
    }
    if (fr_sched_pool_queued(g_http_sched) >= HTTP_NATIVE_QUEUE_HIGH_WATER) {
        fr_sock_close(client);
        return -1;
    }

    http_serve_ctx_t *ctx = (http_serve_ctx_t *)malloc(sizeof(http_serve_ctx_t));
    if (!ctx) {
        fr_sock_close(client);
        return -1;
    }
    ctx->client = client;
    ctx->srv = srv;
    if (fr_sched_pool_submit(g_http_sched, http_serve_task, ctx) != 0) {
        free(ctx);
        fr_sock_close(client);
        return -1;
    }
    return 0;
}

static int accept_one_client(int listen_fd) {
    int client = fr_http_accept_nb(listen_fd);
    if (client >= 0) return client;
#if !defined(FORGE_OS_WINDOWS)
    int err = errno;
    if (fr_sock_would_block(err)) return -1;
    if (err == EMFILE || err == ENFILE) {
        fr_platform_sleep_us(1000);
        return -2;
    }
#endif
    return -1;
}

static void accept_clients(int listen_fd, fr_http_server_state_t *srv) {
    for (int burst = 0; burst < HTTP_ACCEPT_BURST; burst++) {
        int client = accept_one_client(listen_fd);
        if (client == -2) break;
        if (client < 0) break;
        http_submit_client(srv, client);
    }
}

void fr_http_tune_server(void) {
    fr_platform_tune_for_server();
}

#if !defined(FORGE_OS_LINUX)
static void drain_accept_queue(int listen_fd, fr_http_server_state_t *srv) {
    accept_clients(listen_fd, srv);
}
#endif

#if defined(FORGE_OS_LINUX)
static void http_serve_event_loop(int listen_fd, fr_http_server_state_t *srv) {
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
            accept_clients(listen_fd, srv);
        }
    }
}
#elif defined(FORGE_OS_MACOS)
static void http_serve_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    int kq = kqueue();
    if (kq < 0) return;
    fr_sock_set_nonblocking(listen_fd);
    struct kevent change;
    EV_SET(&change, listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(kq, &change, 1, NULL, 0, NULL);
    struct kevent events[64];
    for (;;) {
        int n = kevent(kq, NULL, 0, events, 64, NULL);
        if (n < 0) continue;
        for (int i = 0; i < n; i++) {
            if ((int)events[i].ident == listen_fd) drain_accept_queue(listen_fd, srv);
        }
    }
}
#else
static void http_serve_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    fr_sock_set_nonblocking(listen_fd);
    for (;;) drain_accept_queue(listen_fd, srv);
}
#endif

void fr_http_epoll_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    http_serve_event_loop(listen_fd, srv);
}

void fr_http_serve_forever(int64_t server) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    fr_http_server_state_t *srv = &g_servers[server];
    if (!srv->cached_resp || srv->cached_len == 0) return;
    http_serve_event_loop((int)server, srv);
}

void fr_http_serve_ok(int64_t server, const char *body) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    fr_http_server_state_t *srv = &g_servers[server];
    if (!srv->cached_resp) fr_http_prepare(server, body);
    fr_http_serve_prepared(server);
}

typedef struct {
    int listen_fd;
    fr_http_server_state_t *srv;
    int worker_id;
    void (*loop_fn)(int listen_fd, fr_http_server_state_t *srv);
} http_worker_ctx_t;

static void *http_worker_main(void *arg) {
    http_worker_ctx_t *ctx = (http_worker_ctx_t *)arg;
    int cpus = fr_platform_cpu_count();
    if (cpus > 0) fr_thread_pin_cpu(ctx->worker_id % cpus);
    ctx->loop_fn(ctx->listen_fd, ctx->srv);
    return NULL;
}

void fr_http_spawn_workers(int64_t server, fr_http_server_state_t *srv, int threads,
                           void (*loop_fn)(int listen_fd, fr_http_server_state_t *srv)) {
    if (!srv || !loop_fn) return;
    fr_http_tune_server();

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    int accept_workers = (int)(threads > 0 ? threads : cpus);

    if (accept_workers == 1) {
        loop_fn((int)server, srv);
        return;
    }

    fr_sock_close((int)server);

    http_worker_ctx_t *ctxs = (http_worker_ctx_t *)calloc((size_t)accept_workers, sizeof(http_worker_ctx_t));
    if (!ctxs) {
        loop_fn((int)server, srv);
        return;
    }

    int started = 0;
    for (int i = 0; i < accept_workers; i++) {
        int64_t fd = fr_tcp_listen_reuseport(srv->port);
        if (fd < 0) continue;
        ctxs[started].listen_fd = (int)fd;
        ctxs[started].srv = srv;
        ctxs[started].worker_id = started;
        ctxs[started].loop_fn = loop_fn;
        fr_thread_t *tid = NULL;
        if (fr_thread_start(&tid, http_worker_main, &ctxs[started]) != 0) {
            fr_sock_close((int)fd);
            continue;
        }
        fr_thread_detach(tid);
        started++;
    }

    if (started == 0) {
        free(ctxs);
        loop_fn((int)server, srv);
        return;
    }

    fr_platform_sleep_forever();
}

void fr_http_serve_mt(int64_t server, int64_t threads) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    fr_http_server_state_t *srv = &g_servers[server];
    if (!srv->cached_resp || srv->cached_len == 0) return;
    fr_http_spawn_workers(server, srv, threads, http_serve_event_loop);
}

void fr_http_serve_hybrid(int64_t server, int64_t threads) {
    if (server < 0 || server >= FR_HTTP_MAX_SERVERS) return;
    fr_http_server_state_t *srv = &g_servers[server];
    if (!srv->cached_resp || srv->cached_len == 0) return;

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    int pool_workers = (int)(threads > 0 ? threads : cpus);

    g_http_sched = fr_scheduler_create(pool_workers);
    if (!g_http_sched) {
        fr_http_serve_mt(server, threads);
        return;
    }
    fr_scheduler_start(g_http_sched);
    fr_http_spawn_workers(server, srv, 0, http_serve_event_loop);
}

int fr_http_has_sendfile(void) {
#if defined(FORGE_OS_LINUX)
    return 1;
#else
    return 0;
#endif
}

int fr_http_has_uring(void) {
#if defined(FORGE_HAS_IO_URING)
    return 1;
#else
    return 0;
#endif
}

int fr_http_has_tls(void) {
#if defined(FORGE_HAS_TLS)
    return 1;
#else
    return 0;
#endif
}
