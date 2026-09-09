#ifndef FORGE_HTTP_INTERNAL_H
#define FORGE_HTTP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "forge_runtime.h"

/* Shared plumbing behind the HTTP serving modes.
 *
 * http.c owns the implementations below; http_uring.c, http_tls.c and
 * http_routing.c layer their transport/dispatch specifics on top. Everything
 * that is not mode-specific -- header framing, send-all, connection setup,
 * accept bursts, the readiness loop, and worker-pool handoff -- lives here
 * exactly once so the modes cannot drift apart. */

/* Fixed-size tables indexed by socket fd. */
#define FR_HTTP_MAX_SERVERS 32
#define FR_HTTP_MAX_REQS 128

/* Idle I/O timeout applied to every accepted connection. A client that stops
 * sending mid-request (Slowloris) gets its recv() unblocked and the connection
 * torn down instead of pinning a coroutine/worker forever. */
#define FR_HTTP_IO_TIMEOUT_MS 5000

/* Header buffer size, matching common server defaults (e.g. nginx's
 * large_client_header_buffers): real browser requests exceed 1KB once
 * cookies/User-Agent/Accept-* are included. */
#define FR_HTTP_HEADER_BUF 8192

/* Connections drained per readiness notification before returning to the
 * poller, so one hot listener cannot monopolise the loop. */
#define FR_HTTP_ACCEPT_BURST 256

typedef struct fr_http_server_state {
    int64_t listen_sock;
    int64_t port;
    char *cached_resp;
    size_t cached_len;
    int sendfile_fd;
    size_t sendfile_len;
    void *tls_ctx;
} fr_http_server_state_t;

fr_http_server_state_t *fr_http_state(int64_t server);

/* fr_http_state(), but NULL unless a cached response has been prepared -- the
 * shared precondition of every serve-mode entry point. */
fr_http_server_state_t *fr_http_prepared_state(int64_t server);

/* --- request framing ------------------------------------------------- */

/* First "\r\n\r\n" anywhere in buf[0..len), or NULL.
 *
 * `scan_from` is the length already searched by a previous call; the scan
 * restarts 3 bytes earlier so a terminator straddling two recv() chunks is
 * still found. Pass 0 to scan the whole buffer. Scanning the accumulated
 * buffer rather than only the tail of the latest chunk is what keeps a body
 * that arrived in the same recv() as the headers from being dropped. */
const char *fr_http_find_header_terminator(const char *buf, size_t len, size_t scan_from);

/* Read into buf until the header terminator appears. On success
 * *out_total_len is everything read (headers plus any body bytes that arrived
 * alongside them) and *out_header_len is the offset just past "\r\n\r\n". */
int fr_http_recv_until_headers(int fd, char *buf, size_t cap,
                               size_t *out_total_len, size_t *out_header_len);

/* Read and drop a request head, for modes that answer with a canned
 * response and never inspect the request. */
int fr_http_discard_headers(int client);

/* --- response writing ------------------------------------------------ */

int fr_http_send_all(int fd, const void *data, size_t len);
int fr_http_send_prepared(int client, fr_http_server_state_t *srv);

/* --- connection lifecycle -------------------------------------------- */

/* Put an accepted socket into blocking mode with the idle I/O timeout. */
int fr_http_conn_setup(int client);

/* Whole cached-response exchange on an accepted socket: setup, drain the
 * request head, write the prepared response, close. Always closes `client`. */
void fr_http_serve_cached(int client, fr_http_server_state_t *srv);

/* --- accept / readiness loop ----------------------------------------- */

/* Called with ownership of `client`: the callback must close it. */
typedef void (*fr_http_client_fn)(int client, fr_http_server_state_t *srv);

int fr_http_accept_nb(int listen_fd);

/* Drain up to FR_HTTP_ACCEPT_BURST pending connections, handing each to
 * `on_client`. Backs off briefly on fd exhaustion (EMFILE/ENFILE). */
void fr_http_accept_burst(int listen_fd, fr_http_server_state_t *srv,
                          fr_http_client_fn on_client);

/* Readiness loop over `listen_fd` (epoll on Linux, kqueue on macOS, busy
 * accept elsewhere), draining an accept burst per notification. Never
 * returns except on unrecoverable poller setup failure. */
void fr_http_event_loop(int listen_fd, fr_http_server_state_t *srv,
                        fr_http_client_fn on_client);

/* fr_http_event_loop bound to the default cached-response handler; used as
 * the fallback when a mode's own transport is unavailable. */
void fr_http_epoll_event_loop(int listen_fd, fr_http_server_state_t *srv);

/* --- worker pools ---------------------------------------------------- */

void fr_http_tune_server(void);

/* Run `loop_fn` across `threads` accept workers (0 => one per CPU), each on
 * its own SO_REUSEPORT listener pinned to a CPU. Does not return. */
void fr_http_spawn_workers(int64_t server, fr_http_server_state_t *srv, int threads,
                           void (*loop_fn)(int listen_fd, fr_http_server_state_t *srv));

/* Unit of work handed to a native worker pool. */
typedef struct {
    int client;
    fr_http_server_state_t *srv;
} fr_http_job_t;

#define FR_HTTP_SUBMIT_OK 0
/* No pool configured -- the caller decides whether to run inline or drop.
 * `client` is still open and still owned by the caller. */
#define FR_HTTP_SUBMIT_NO_POOL 1
/* Overloaded or out of memory: `client` has already been closed. */
#define FR_HTTP_SUBMIT_DROPPED (-1)

/* Hand {client, srv} to `sched` as a heap-allocated fr_http_job_t. `run`
 * receives that job and must free it. */
int fr_http_pool_submit(fr_scheduler_t *sched, size_t high_water,
                        void (*run)(void *job), int client,
                        fr_http_server_state_t *srv);

#endif
