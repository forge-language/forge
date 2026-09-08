#include "forge/http.h"
#include "http_internal.h"
#include "forge/tcp.h"
#include "forge/platform.h"
#include "forge/thread.h"
#include "forge_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(FORGE_HAS_TLS)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <errno.h>

#if defined(FORGE_OS_LINUX)
#include <sys/epoll.h>
#include <unistd.h>
#endif

#define TLS_ACCEPT_BURST 256
#define TLS_NATIVE_QUEUE_HIGH_WATER 1024
#define TLS_IO_TIMEOUT_MS 5000

static int g_tls_inited = 0;
static fr_scheduler_t *g_tls_sched;

static void tls_init_once(void) {
    if (g_tls_inited) return;
    OPENSSL_init_ssl(0, NULL);
    g_tls_inited = 1;
}

typedef struct {
    SSL_CTX *ctx;
} fr_http_tls_store_t;

static fr_http_tls_store_t g_tls[32];

int64_t fr_http_listen_tls(int64_t port, const char *cert, const char *key) {
    tls_init_once();
    if (!cert || !key) return -1;
    int64_t sock = fr_http_listen(port);
    if (sock < 0) return -1;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fr_http_server_close(sock);
        return -1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_SINGLE_ECDH_USE | SSL_OP_SINGLE_DH_USE);
#if defined(TLS1_3_VERSION)
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384");
#endif
    SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256");
    if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        fr_http_server_close(sock);
        return -1;
    }
    g_tls[sock].ctx = ctx;
    fr_http_state(sock)->tls_ctx = ctx;
    return sock;
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int tls_handshake(SSL *ssl) {
    int64_t deadline = monotonic_ms() + TLS_IO_TIMEOUT_MS;
    for (;;) {
        int rc = SSL_accept(ssl);
        if (rc == 1) return 0;
        int err = SSL_get_error(ssl, rc);
        if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
            monotonic_ms() < deadline) continue;
        return -1;
    }
}

static int tls_discard_headers(SSL *ssl) {
    char buf[512];
    size_t len = 0;
    int64_t deadline = monotonic_ms() + TLS_IO_TIMEOUT_MS;
    while (len + 1 < sizeof(buf)) {
        int n = SSL_read(ssl, buf + len, (int)(sizeof(buf) - len - 1));
        if (n > 0) {
            len += (size_t)n;
            buf[len] = '\0';
            if (len >= 4 && buf[len - 4] == '\r' && buf[len - 3] == '\n' &&
                buf[len - 2] == '\r' && buf[len - 1] == '\n') {
                return 0;
            }
            continue;
        }
        int err = SSL_get_error(ssl, n);
        if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
            monotonic_ms() < deadline) continue;
        return -1;
    }
    return -1;
}

static int tls_send_all(SSL *ssl, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    int64_t deadline = monotonic_ms() + TLS_IO_TIMEOUT_MS;
    while (sent < len) {
        int n = SSL_write(ssl, p + sent, (int)(len - sent));
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        int err = SSL_get_error(ssl, n);
        if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
            monotonic_ms() < deadline) continue;
        return -1;
    }
    return 0;
}

static void tls_close(SSL *ssl, int client) {
    if (ssl) {
        SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
        SSL_free(ssl);
    }
    fr_sock_close(client);
}

typedef struct {
    int client;
    fr_http_server_state_t *srv;
    SSL_CTX *ctx;
} tls_serve_ctx_t;

static void serve_tls_client(fr_http_server_state_t *srv, int client, SSL_CTX *ctx) {
    if (fr_sock_set_blocking(client) != 0 ||
        fr_sock_set_timeout(client, TLS_IO_TIMEOUT_MS) != 0) {
        fr_sock_close(client);
        return;
    }
    fr_sock_set_tcp_nodelay(client);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        fr_sock_close(client);
        return;
    }
    SSL_set_fd(ssl, client);
    if (tls_handshake(ssl) < 0) {
        tls_close(ssl, client);
        return;
    }
    if (tls_discard_headers(ssl) < 0) {
        tls_close(ssl, client);
        return;
    }
    if (!srv->cached_resp || srv->cached_len == 0 ||
        tls_send_all(ssl, srv->cached_resp, srv->cached_len) < 0) {
        tls_close(ssl, client);
        return;
    }
    tls_close(ssl, client);
}

static void tls_serve_task(void *arg) {
    tls_serve_ctx_t *ctx = (tls_serve_ctx_t *)arg;
    if (!ctx) return;
    serve_tls_client(ctx->srv, ctx->client, ctx->ctx);
    free(ctx);
}

static int tls_pool_workers(int requested, int cpus) {
    if (requested > 0) return requested;
    int n = cpus * 4;
    if (n < 32) n = 32;
    if (n > 256) n = 256;
    return n;
}

static int tls_submit_client(fr_http_server_state_t *srv, int client, SSL_CTX *ctx) {
    if (!g_tls_sched) {
        serve_tls_client(srv, client, ctx);
        return 0;
    }
    if (fr_sched_pool_queued(g_tls_sched) >= TLS_NATIVE_QUEUE_HIGH_WATER) {
        fr_sock_close(client);
        return -1;
    }

    tls_serve_ctx_t *job = (tls_serve_ctx_t *)malloc(sizeof(tls_serve_ctx_t));
    if (!job) {
        fr_sock_close(client);
        return -1;
    }
    job->client = client;
    job->srv = srv;
    job->ctx = ctx;
    if (fr_sched_pool_submit(g_tls_sched, tls_serve_task, job) != 0) {
        free(job);
        fr_sock_close(client);
        return -1;
    }
    return 0;
}

static int tls_accept_one(int listen_fd) {
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

static void accept_tls_clients(int listen_fd, fr_http_server_state_t *srv) {
    SSL_CTX *ctx = (SSL_CTX *)srv->tls_ctx;
    if (!ctx) return;
    for (int burst = 0; burst < TLS_ACCEPT_BURST; burst++) {
        int client = tls_accept_one(listen_fd);
        if (client == -2) break;
        if (client < 0) break;
        tls_submit_client(srv, client, ctx);
    }
}

static void http_tls_event_loop(int listen_fd, fr_http_server_state_t *srv) {
#if defined(FORGE_OS_LINUX)
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
            accept_tls_clients(listen_fd, srv);
        }
    }
#else
    fr_sock_set_nonblocking(listen_fd);
    for (;;) accept_tls_clients(listen_fd, srv);
#endif
}

void fr_http_serve_tls_mt(int64_t server, int64_t threads) {
    if (server < 0) return;
    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv || !srv->cached_resp || srv->cached_len == 0 || !srv->tls_ctx) return;

    fr_http_tune_server();

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    int pool_workers = tls_pool_workers((int)threads, cpus);

    g_tls_sched = fr_scheduler_create(pool_workers);
    if (g_tls_sched) fr_scheduler_start(g_tls_sched);

    fr_http_spawn_workers(server, srv, threads, http_tls_event_loop);
}

#else

int64_t fr_http_listen_tls(int64_t port, const char *cert, const char *key) {
    (void)port;
    (void)cert;
    (void)key;
    return -1;
}

void fr_http_serve_tls_mt(int64_t server, int64_t threads) {
    (void)server;
    (void)threads;
}

#endif
