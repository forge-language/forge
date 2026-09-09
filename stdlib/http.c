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

/* Backlog of native tasks past which new connections are shed rather than
 * queued, so an overloaded server fails fast instead of growing an unbounded
 * queue of connections it will answer long after the client gave up. */
#define HTTP_NATIVE_QUEUE_HIGH_WATER 2048
#define FR_HTTP_MAX_BODY (8 * 1024 * 1024)

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

fr_http_server_state_t *fr_http_prepared_state(int64_t server) {
    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv || !srv->cached_resp || srv->cached_len == 0) return NULL;
    return srv;
}

/* ====================================================================== *
 * Request framing (shared by every serving mode)
 * ====================================================================== */

const char *fr_http_find_header_terminator(const char *buf, size_t len, size_t scan_from) {
    if (len < 4) return NULL;
    /* Resume 3 bytes before the already-scanned region so a terminator
     * straddling two recv() chunks is still found; this keeps the scan
     * linear in total bytes read instead of rescanning the whole buffer on
     * every chunk (which a client trickling one byte at a time could turn
     * into quadratic work). */
    const char *p = buf + (scan_from > 3 ? scan_from - 3 : 0);
    const char *last = buf + len - 4; /* last offset a match can start at */
    while (p <= last) {
        const char *cr = (const char *)memchr(p, '\r', (size_t)(last - p) + 1);
        if (!cr) return NULL;
        if (cr[1] == '\n' && cr[2] == '\r' && cr[3] == '\n') return cr;
        p = cr + 1;
    }
    return NULL;
}

int fr_http_recv_until_headers(int fd, char *buf, size_t cap,
                               size_t *out_total_len, size_t *out_header_len) {
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t n = fr_sock_recv(fd, buf + len, cap - len - 1);
        if (n < 0) {
#if !defined(FORGE_OS_WINDOWS)
            /* A signal is not a client failure. The SO_RCVTIMEO idle timeout
             * surfaces as EAGAIN/EWOULDBLOCK, not EINTR, so retrying here
             * does not weaken the Slowloris cutoff. */
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (n == 0) return -1; /* peer closed before the head was complete */
        size_t scanned = len;
        len += (size_t)n;
        buf[len] = '\0';
        const char *term = fr_http_find_header_terminator(buf, len, scanned);
        if (term) {
            if (out_total_len) *out_total_len = len;
            if (out_header_len) *out_header_len = (size_t)(term - buf) + 4;
            return 0;
        }
    }
    return -1; /* head larger than the buffer */
}

int fr_http_discard_headers(int client) {
    char buf[FR_HTTP_HEADER_BUF];
    return fr_http_recv_until_headers(client, buf, sizeof(buf), NULL, NULL);
}

/* ====================================================================== *
 * Response writing
 * ====================================================================== */

int fr_http_send_all(int fd, const void *data, size_t len) {
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
    return fr_http_send_all(client, srv->cached_resp, srv->cached_len);
}

/* ====================================================================== *
 * Connection lifecycle
 * ====================================================================== */

int fr_http_conn_setup(int client) {
    if (fr_sock_set_blocking(client) != 0) return -1;
    return fr_sock_set_timeout(client, FR_HTTP_IO_TIMEOUT_MS);
}

void fr_http_serve_cached(int client, fr_http_server_state_t *srv) {
    if (fr_http_conn_setup(client) == 0 && fr_http_discard_headers(client) == 0) {
        fr_http_send_prepared(client, srv);
    }
    fr_sock_close(client);
}

/* ====================================================================== *
 * Accept bursts and the readiness loop
 * ====================================================================== */

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

void fr_http_accept_burst(int listen_fd, fr_http_server_state_t *srv,
                          fr_http_client_fn on_client) {
    for (int burst = 0; burst < FR_HTTP_ACCEPT_BURST; burst++) {
        int client = fr_http_accept_nb(listen_fd);
        if (client < 0) {
#if !defined(FORGE_OS_WINDOWS)
            /* Out of descriptors: pause briefly so the loop does not spin at
             * 100% CPU re-failing the same accept() until one is released. */
            if (errno == EMFILE || errno == ENFILE) fr_platform_sleep_us(1000);
#endif
            return;
        }
        on_client(client, srv);
    }
}

#if defined(FORGE_OS_LINUX)
#define HTTP_POLL_EVENTS 128

void fr_http_event_loop(int listen_fd, fr_http_server_state_t *srv,
                        fr_http_client_fn on_client) {
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

    struct epoll_event events[HTTP_POLL_EVENTS];
    for (;;) {
        int n = epoll_wait(epfd, events, HTTP_POLL_EVENTS, -1);
        if (n < 0) continue; /* EINTR and transient errors alike: re-poll */
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                fr_http_accept_burst(listen_fd, srv, on_client);
            }
        }
    }
}
#elif defined(FORGE_OS_MACOS)
#define HTTP_POLL_EVENTS 64

void fr_http_event_loop(int listen_fd, fr_http_server_state_t *srv,
                        fr_http_client_fn on_client) {
    int kq = kqueue();
    if (kq < 0) return;
    fr_sock_set_nonblocking(listen_fd);

    struct kevent change;
    EV_SET(&change, listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(kq, &change, 1, NULL, 0, NULL);

    struct kevent events[HTTP_POLL_EVENTS];
    for (;;) {
        int n = kevent(kq, NULL, 0, events, HTTP_POLL_EVENTS, NULL);
        if (n < 0) continue;
        for (int i = 0; i < n; i++) {
            if ((int)events[i].ident == listen_fd) {
                fr_http_accept_burst(listen_fd, srv, on_client);
            }
        }
    }
}
#else
void fr_http_event_loop(int listen_fd, fr_http_server_state_t *srv,
                        fr_http_client_fn on_client) {
    fr_sock_set_nonblocking(listen_fd);
    for (;;) fr_http_accept_burst(listen_fd, srv, on_client);
}
#endif

/* ====================================================================== *
 * Native worker-pool handoff
 * ====================================================================== */

int fr_http_pool_submit(fr_scheduler_t *sched, size_t high_water,
                        void (*run)(void *job), int client,
                        fr_http_server_state_t *srv) {
    if (!sched) return FR_HTTP_SUBMIT_NO_POOL;
    if (fr_sched_pool_queued(sched) >= high_water) {
        fr_sock_close(client);
        return FR_HTTP_SUBMIT_DROPPED;
    }

    fr_http_job_t *job = (fr_http_job_t *)malloc(sizeof(*job));
    if (!job) {
        fr_sock_close(client);
        return FR_HTTP_SUBMIT_DROPPED;
    }
    job->client = client;
    job->srv = srv;
    if (fr_sched_pool_submit(sched, run, job) != 0) {
        free(job);
        fr_sock_close(client);
        return FR_HTTP_SUBMIT_DROPPED;
    }
    return FR_HTTP_SUBMIT_OK;
}

/* ====================================================================== *
 * Client side: fr_http_get / fr_http_post
 * ====================================================================== */

static void parse_url(const char *url, char *host, size_t hcap, int64_t *port,
                      char *path, size_t pcap) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    int has_port = colon && (!slash || colon < slash);

    size_t hlen;
    if (has_port) hlen = (size_t)(colon - p);
    else hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen >= hcap) hlen = hcap - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = has_port ? atoll(colon + 1) : 80;
    snprintf(path, pcap, "%s", slash ? slash : "/");
}

static char *http_exchange(const char *method, const char *url, const char *body) {
    char host[256], path[512];
    int64_t port = 80;
    parse_url(url, host, sizeof(host), &port, path, sizeof(path));

    int64_t sock = fr_tcp_connect(host, port);
    if (sock < 0) return NULL;

    int has_body = body && body[0];
    char header[1024];
    if (has_body) {
        snprintf(header, sizeof(header),
                 "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 method, path, host, strlen(body));
    } else {
        snprintf(header, sizeof(header),
                 "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 method, path, host);
    }
    fr_tcp_send(sock, header);
    if (has_body) fr_tcp_send(sock, body);

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

/* ====================================================================== *
 * Server side: accept / inspect / respond
 * ====================================================================== */

int64_t fr_http_listen(int64_t port) {
    int64_t sock = fr_tcp_listen(port);
    if (sock < 0) return -1;
    if (sock >= FR_HTTP_MAX_SERVERS) {
        fr_tcp_close(sock);
        return -1;
    }
    fr_http_server_state_t *srv = &g_servers[sock];
    memset(srv, 0, sizeof(*srv));
    srv->listen_sock = sock;
    srv->port = port;
    srv->sendfile_fd = -1;
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

/* Case-insensitive lookup of "Name:" within headers[0..header_len), returning
 * the value with leading whitespace skipped, or NULL. Only line starts are
 * considered, so a header name appearing inside another header's value cannot
 * match. */
static const char *find_header_value(const char *headers, size_t header_len,
                                     const char *name) {
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + header_len;
    for (;;) {
        if ((size_t)(end - p) >= name_len && header_ci_equal(p, name, name_len)) {
            const char *v = p + name_len;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) return NULL;
        p = nl + 1;
    }
}

/* raw[0..total_len) holds everything read so far; raw[0..header_len) is the
 * head (including the trailing "\r\n\r\n"); raw[header_len..total_len) is
 * whatever body bytes already arrived in the same recv()s. If Content-Length
 * says there is more body than that, the remainder is read from fd (which
 * already carries the idle-read timeout). */
static void parse_http_request(int fd, const char *raw, size_t total_len,
                               size_t header_len, fr_http_req_t *req) {
    req->method[0] = req->path[0] = '\0';
    req->body = NULL;
    req->arena = NULL;

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

    const char *body_start = raw + header_len;
    size_t body_have = total_len - header_len;
    /* Without a Content-Length there is nothing more to read, so the body is
     * exactly what already arrived. */
    size_t body_len = content_length > 0 ? (size_t)content_length : body_have;
    if (body_len > FR_HTTP_MAX_BODY) body_len = FR_HTTP_MAX_BODY;
    if (body_len == 0) return;

    /* Lazily-created, per-request arena (see fr_http_req_t::arena): scoped to
     * exactly this request, so it can never be reset out from under a
     * different still-live request sharing the same OS thread. */
    req->arena = fr_arena_create(0);
    if (!req->arena) return;
    char *buf = (char *)fr_arena_alloc(req->arena, body_len + 1, 1);
    if (!buf) return;

    size_t got = body_have < body_len ? body_have : body_len;
    memcpy(buf, body_start, got);
    while (got < body_len) {
        ssize_t n = fr_sock_recv(fd, buf + got, body_len - got);
        if (n <= 0) break; /* idle timeout or peer closed early; keep what we have */
        got += (size_t)n;
    }
    buf[got] = '\0';
    req->body = buf;
}

int64_t fr_http_accept(int64_t server) {
    int64_t client = fr_tcp_accept(server);
    if (client < 0) return -1;
    if (client >= FR_HTTP_MAX_REQS) {
        fr_tcp_close(client);
        return -1;
    }
    if (fr_http_conn_setup((int)client) != 0) {
        fr_tcp_close(client);
        return -1;
    }

    char buf[FR_HTTP_HEADER_BUF];
    size_t total_len = 0, header_len = 0;
    if (fr_http_recv_until_headers((int)client, buf, sizeof(buf),
                                   &total_len, &header_len) < 0) {
        fr_tcp_close(client);
        return -1;
    }

    g_reqs[client].sock = client;
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

    int sock = (int)g_reqs[req].sock;
    /* Coalesce head and body into one write when they fit, so the common
     * small response costs a single syscall. */
    if (blen > 0 && (size_t)hlen + blen < sizeof(out)) {
        memcpy(out + hlen, body, blen);
        fr_http_send_all(sock, out, (size_t)hlen + blen);
        return;
    }
    if (fr_http_send_all(sock, out, (size_t)hlen) == 0 && blen > 0) {
        fr_http_send_all(sock, body, blen);
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
    fr_http_server_state_t *srv = fr_http_state(server);
    if (srv) {
        free(srv->cached_resp);
        srv->cached_resp = NULL;
        srv->cached_len = 0;
        if (srv->sendfile_fd >= 0) {
            close(srv->sendfile_fd);
            srv->sendfile_fd = -1;
            srv->sendfile_len = 0;
        }
    }
    fr_tcp_close(server);
}

/* ====================================================================== *
 * Prepared (cached) responses
 * ====================================================================== */

void fr_http_prepare(int64_t server, const char *body) {
    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv) return;
    free(srv->cached_resp);
    srv->cached_resp = NULL;
    srv->cached_len = 0;

    size_t blen = body ? strlen(body) : 0;
    size_t cap = 128 + blen;
    char *out = (char *)malloc(cap);
    if (!out) return;
    int n = snprintf(out, cap,
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%.*s",
                     blen, (int)blen, body ? body : "");
    if (n <= 0) { free(out); return; }
    srv->cached_resp = out;
    srv->cached_len = (size_t)n;
}

void fr_http_prepare_sendfile(int64_t server, const char *body) {
    fr_http_prepare(server, body);
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv) return;

#if defined(FORGE_OS_LINUX)
    if (srv->sendfile_fd >= 0) {
        close(srv->sendfile_fd);
        srv->sendfile_fd = -1;
        srv->sendfile_len = 0;
    }
    int fd = (int)memfd_create("forge_http_resp", MFD_CLOEXEC);
    if (fd < 0) return;
    size_t wrote = 0;
    while (wrote < srv->cached_len) {
        ssize_t n = write(fd, srv->cached_resp + wrote, srv->cached_len - wrote);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return;
        }
        wrote += (size_t)n;
    }
    srv->sendfile_fd = fd;
    srv->sendfile_len = srv->cached_len;
#endif
}

void fr_http_serve_prepared(int64_t server) {
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv) return;

    int client = (int)fr_tcp_accept(server);
    if (client < 0) return;
    fr_http_serve_cached(client, srv);
}

void fr_http_serve_ok(int64_t server, const char *body) {
    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv) return;
    if (!srv->cached_resp) fr_http_prepare(server, body);
    fr_http_serve_prepared(server);
}

/* ====================================================================== *
 * Serving modes
 * ====================================================================== */

void fr_http_tune_server(void) {
    fr_platform_tune_for_server();
}

/* Scheduler backing the hybrid mode. NULL in the pure event-loop modes, in
 * which case connections are served inline on the accept thread. */
static fr_scheduler_t *g_http_sched;

static void http_serve_task(void *arg) {
    fr_http_job_t *job = (fr_http_job_t *)arg;
    if (!job) return;
    fr_http_serve_cached(job->client, job->srv);
    free(job);
}

static void http_dispatch_client(int client, fr_http_server_state_t *srv) {
    if (fr_http_pool_submit(g_http_sched, HTTP_NATIVE_QUEUE_HIGH_WATER,
                            http_serve_task, client, srv) == FR_HTTP_SUBMIT_NO_POOL) {
        fr_http_serve_cached(client, srv);
    }
}

void fr_http_epoll_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    fr_http_event_loop(listen_fd, srv, http_dispatch_client);
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
    int accept_workers = threads > 0 ? threads : cpus;

    if (accept_workers == 1) {
        loop_fn((int)server, srv);
        return;
    }

    /* Each worker gets its own SO_REUSEPORT listener so the kernel spreads
     * incoming connections instead of every worker contending on one queue;
     * the original listener is therefore retired here. */
    fr_sock_close((int)server);

    http_worker_ctx_t *ctxs =
        (http_worker_ctx_t *)calloc((size_t)accept_workers, sizeof(http_worker_ctx_t));
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

void fr_http_serve_forever(int64_t server) {
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv) return;
    fr_http_epoll_event_loop((int)server, srv);
}

void fr_http_serve_mt(int64_t server, int64_t threads) {
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv) return;
    fr_http_spawn_workers(server, srv, (int)threads, fr_http_epoll_event_loop);
}

void fr_http_serve_hybrid(int64_t server, int64_t threads) {
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv) return;

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    int pool_workers = threads > 0 ? (int)threads : cpus;

    g_http_sched = fr_scheduler_create(pool_workers);
    if (!g_http_sched) {
        fr_http_serve_mt(server, threads);
        return;
    }
    fr_scheduler_start(g_http_sched);
    /* threads == 0: one accept worker per CPU, each feeding the shared pool. */
    fr_http_spawn_workers(server, srv, 0, fr_http_epoll_event_loop);
}

/* ====================================================================== *
 * Build capability flags
 * ====================================================================== */

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
