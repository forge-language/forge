#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "forge/http.h"
#include "http_internal.h"
#include "forge/platform.h"
#include "forge/tcp.h"
#include "forge/thread.h"
#include "forge_runtime.h"
#include <stdlib.h>
#include <string.h>

#if defined(FORGE_HAS_IO_URING) && defined(FORGE_OS_LINUX)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <liburing.h>
#include <unistd.h>

#define URING_QUEUE_DEPTH 64
#define URING_QUEUE_HIGH_WATER 1024

static fr_scheduler_t *g_uring_sched;

static int uring_wait_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_out) {
    for (;;) {
        struct io_uring_cqe *cqe = NULL;
        int ret = io_uring_wait_cqe(ring, &cqe);
        if (ret == 0 && cqe) {
            *cqe_out = cqe;
            return 0;
        }
        if (ret == -EINTR) continue;
        return -1;
    }
}

static int uring_accept_one(struct io_uring *ring, int listen_fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) return -1;
    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, (void *)1);
    if (io_uring_submit(ring) < 0) return -1;
    struct io_uring_cqe *cqe = NULL;
    if (uring_wait_cqe(ring, &cqe) < 0) return -1;
    int res = cqe->res;
    io_uring_cqe_seen(ring, cqe);
    return res;
}

typedef struct {
    int client;
    fr_http_server_state_t *srv;
} uring_serve_ctx_t;

static void uring_serve_task(void *arg) {
    uring_serve_ctx_t *ctx = (uring_serve_ctx_t *)arg;
    if (!ctx) return;
    int client = ctx->client;
    fr_http_server_state_t *srv = ctx->srv;
    free(ctx);

    if (fr_sock_set_blocking(client) != 0 ||
        fr_sock_set_timeout(client, 5000) != 0 ||
        fr_http_discard_headers(client) < 0 ||
        fr_http_send_prepared(client, srv) < 0) {
        fr_sock_close(client);
        return;
    }
    fr_sock_close(client);
}

static int submit_uring_client(fr_http_server_state_t *srv, int client) {
    if (!g_uring_sched ||
        fr_sched_pool_queued(g_uring_sched) >= URING_QUEUE_HIGH_WATER) {
        fr_sock_close(client);
        return -1;
    }
    uring_serve_ctx_t *ctx = (uring_serve_ctx_t *)malloc(sizeof(*ctx));
    if (!ctx) {
        fr_sock_close(client);
        return -1;
    }
    ctx->client = client;
    ctx->srv = srv;
    if (fr_sched_pool_submit(g_uring_sched, uring_serve_task, ctx) != 0) {
        free(ctx);
        fr_sock_close(client);
        return -1;
    }
    return 0;
}

static void http_uring_event_loop(int listen_fd, fr_http_server_state_t *srv) {
    fr_http_tune_server();
    struct io_uring ring;
    if (io_uring_queue_init(URING_QUEUE_DEPTH, &ring, 0) < 0) {
        fr_http_epoll_event_loop(listen_fd, srv);
        return;
    }
    fr_sock_set_nonblocking(listen_fd);

    for (;;) {
        int client = uring_accept_one(&ring, listen_fd);
        if (client < 0) continue;
        submit_uring_client(srv, client);
    }
}

void fr_http_serve_uring(int64_t server, int64_t threads) {
    if (server < 0) return;
    fr_http_server_state_t *srv = fr_http_state(server);
    if (!srv || !srv->cached_resp || srv->cached_len == 0) return;

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    int workers = threads > 0 ? (int)threads : cpus * 2;
    g_uring_sched = fr_scheduler_create(workers);
    if (!g_uring_sched) {
        fr_http_serve_mt(server, threads);
        return;
    }
    fr_scheduler_start(g_uring_sched);
    fr_http_spawn_workers(server, srv, threads, http_uring_event_loop);
}

#else

void fr_http_serve_uring(int64_t server, int64_t threads) {
    fr_http_serve_mt(server, threads);
}

#endif
