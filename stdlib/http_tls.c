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

#define TLS_NATIVE_QUEUE_HIGH_WATER 1024

static int g_tls_inited = 0;
static fr_scheduler_t *g_tls_sched;

static void tls_init_once(void) {
    if (g_tls_inited) return;
    OPENSSL_init_ssl(0, NULL);
    g_tls_inited = 1;
}

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
    fr_http_state(sock)->tls_ctx = ctx;
    return sock;
}

/* --- TLS I/O with a wall-clock budget ---------------------------------
 *
 * SO_RCVTIMEO alone is not enough here: a single SSL_read can loop over many
 * socket reads during renegotiation/handshake, so each operation carries its
 * own deadline. Same purpose as the plain path's idle timeout -- a client
 * that stalls mid-exchange is dropped rather than pinning a pool worker. */

static int64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* True while `rc` is a retryable want-read/want-write and time remains. */
static int tls_should_retry(SSL *ssl, int rc, int64_t deadline) {
    int err = SSL_get_error(ssl, rc);
    return (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
           monotonic_ms() < deadline;
}

static int tls_handshake(SSL *ssl) {
    int64_t deadline = monotonic_ms() + FR_HTTP_IO_TIMEOUT_MS;
    for (;;) {
        int rc = SSL_accept(ssl);
        if (rc == 1) return 0;
        if (!tls_should_retry(ssl, rc, deadline)) return -1;
    }
}

static int tls_discard_headers(SSL *ssl) {
    char buf[FR_HTTP_HEADER_BUF];
    size_t len = 0;
    int64_t deadline = monotonic_ms() + FR_HTTP_IO_TIMEOUT_MS;
    while (len + 1 < sizeof(buf)) {
        int n = SSL_read(ssl, buf + len, (int)(sizeof(buf) - len - 1));
        if (n <= 0) {
            if (!tls_should_retry(ssl, n, deadline)) return -1;
            continue;
        }
        size_t scanned = len;
        len += (size_t)n;
        /* Whole-buffer scan (resuming across the chunk boundary), matching
         * the plain path: a terminator that is not the last four bytes of
         * this particular SSL_read must still end the head. */
        if (fr_http_find_header_terminator(buf, len, scanned)) return 0;
    }
    return -1;
}

static int tls_send_all(SSL *ssl, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t sent = 0;
    int64_t deadline = monotonic_ms() + FR_HTTP_IO_TIMEOUT_MS;
    while (sent < len) {
        int n = SSL_write(ssl, p + sent, (int)(len - sent));
        if (n <= 0) {
            if (!tls_should_retry(ssl, n, deadline)) return -1;
            continue;
        }
        sent += (size_t)n;
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

static void serve_tls_client(int client, fr_http_server_state_t *srv) {
    SSL_CTX *ctx = (SSL_CTX *)srv->tls_ctx;
    if (!ctx || fr_http_conn_setup(client) != 0) {
        fr_sock_close(client);
        return;
    }
    /* Handshake and response are several small writes; without this they
     * would be held back by Nagle waiting on the peer's ACK. */
    fr_sock_set_tcp_nodelay(client);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        fr_sock_close(client);
        return;
    }
    SSL_set_fd(ssl, client);
    if (tls_handshake(ssl) == 0 && tls_discard_headers(ssl) == 0 &&
        srv->cached_resp && srv->cached_len > 0) {
        tls_send_all(ssl, srv->cached_resp, srv->cached_len);
    }
    tls_close(ssl, client);
}

static void tls_serve_task(void *arg) {
    fr_http_job_t *job = (fr_http_job_t *)arg;
    if (!job) return;
    serve_tls_client(job->client, job->srv);
    free(job);
}

static void tls_dispatch_client(int client, fr_http_server_state_t *srv) {
    if (fr_http_pool_submit(g_tls_sched, TLS_NATIVE_QUEUE_HIGH_WATER,
                            tls_serve_task, client, srv) == FR_HTTP_SUBMIT_NO_POOL) {
        serve_tls_client(client, srv);
    }
}

static void http_tls_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    fr_http_event_loop(listen_fd, srv, tls_dispatch_client);
}

/* Handshakes are CPU-heavy and block their worker, so the pool is sized well
 * above the CPU count -- unlike the plain path, throughput here is bounded by
 * concurrent handshakes in flight, not by cores. */
static int tls_pool_workers(int requested, int cpus) {
    if (requested > 0) return requested;
    int n = cpus * 4;
    if (n < 32) n = 32;
    if (n > 256) n = 256;
    return n;
}

void fr_http_serve_tls_mt(int64_t server, int64_t threads) {
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv || !srv->tls_ctx) return;

    fr_http_tune_server();

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    g_tls_sched = fr_scheduler_create(tls_pool_workers((int)threads, cpus));
    if (g_tls_sched) fr_scheduler_start(g_tls_sched);

    fr_http_spawn_workers(server, srv, (int)threads, http_tls_event_loop);
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
