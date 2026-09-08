#ifndef FORGE_HTTP_INTERNAL_H
#define FORGE_HTTP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

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

int fr_http_discard_headers(int client);
int fr_http_send_prepared(int client, fr_http_server_state_t *srv);
int fr_http_accept_nb(int listen_fd);
void fr_http_tune_server(void);
void fr_http_epoll_event_loop(int listen_fd, fr_http_server_state_t *srv);
void fr_http_spawn_workers(int64_t server, fr_http_server_state_t *srv, int threads,
                           void (*loop_fn)(int listen_fd, fr_http_server_state_t *srv));

#endif
