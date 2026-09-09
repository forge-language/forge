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

static void uring_serve_task(void *arg) {
    fr_http_job_t *job = (fr_http_job_t *)arg;
    if (!job) return;
    fr_http_serve_cached(job->client, job->srv);
    free(job);
}

static void uring_dispatch_client(int client, fr_http_server_state_t *srv) {
    /* This mode exists to keep the accept loop free of per-request work, so
     * with no pool to hand off to there is nothing useful to do but shed the
     * connection rather than block the uring loop on it. */
    if (fr_http_pool_submit(g_uring_sched, URING_QUEUE_HIGH_WATER,
                            uring_serve_task, client, srv) == FR_HTTP_SUBMIT_NO_POOL) {
        fr_sock_close(client);
    }
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
        uring_dispatch_client(client, srv);
    }
}

void fr_http_serve_uring(int64_t server, int64_t threads) {
    fr_http_server_state_t *srv = fr_http_prepared_state(server);
    if (!srv) return;

    int cpus = fr_platform_cpu_count();
    if (cpus < 1) cpus = 1;
    int workers = threads > 0 ? (int)threads : cpus * 2;
    g_uring_sched = fr_scheduler_create(workers);
    if (!g_uring_sched) {
        fr_http_serve_mt(server, threads);
        return;
    }
    fr_scheduler_start(g_uring_sched);
    fr_http_spawn_workers(server, srv, (int)threads, http_uring_event_loop);
}

#else

void fr_http_serve_uring(int64_t server, int64_t threads) {
    fr_http_serve_mt(server, threads);
}

#endif
