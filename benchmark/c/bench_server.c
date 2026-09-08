/* Minimal HTTP server for Forge vs C benchmark. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_port = 19084;
static int g_threads = 0;

static const char RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 12\r\n"
    "Connection: close\r\n\r\n"
    "Hello, World";

static void serve_client(int client) {
    char buf[4096];
    recv(client, buf, sizeof(buf), 0);
    send(client, RESPONSE, sizeof(RESPONSE) - 1, MSG_NOSIGNAL);
    close(client);
}

static int make_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((uint16_t)g_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(fd, 1024) < 0) {
        perror("listen");
        exit(1);
    }
    return fd;
}

static void *worker_main(void *arg) {
    (void)arg;
    int listen_fd = make_listener();
    for (;;) {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        serve_client(client);
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *port_env = getenv("PORT");
    if (port_env && port_env[0]) g_port = atoi(port_env);

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    g_threads = cpus > 0 ? (int)cpus : 4;

    printf("C benchmark server on port %d (%d threads)\n", g_port, g_threads);
    fflush(stdout);

    pthread_t *tids = malloc(sizeof(pthread_t) * (size_t)g_threads);
    for (int i = 0; i < g_threads; i++) {
        pthread_create(&tids[i], NULL, worker_main, NULL);
    }
    for (int i = 0; i < g_threads; i++) {
        pthread_join(tids[i], NULL);
    }
    free(tids);
    return 0;
}
