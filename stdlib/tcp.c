#include "forge/tcp.h"
#include "forge/platform.h"
#include <stdlib.h>
#include <string.h>

#if !defined(FORGE_OS_WINDOWS)
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

void fr_net_init(void) {
    fr_platform_init();
}

static int set_reuseaddr(fr_socket_t fd) {
    int yes = 1;
#if defined(FORGE_OS_WINDOWS)
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#else
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
}

static int set_reuseport(fr_socket_t fd) {
#if defined(FORGE_OS_LINUX) || defined(FORGE_OS_MACOS)
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#else
    (void)fd;
    return 0;
#endif
}

static void tune_listen_socket(fr_socket_t fd) {
#if defined(FORGE_OS_LINUX) || defined(FORGE_OS_MACOS)
    int buf = 65536;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
#endif
}

static int64_t tcp_listen_common(int64_t port, int reuseport) {
    fr_platform_init();
    fr_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == FR_SOCK_INVALID) return -1;
    set_reuseaddr(fd);
    if (reuseport) set_reuseport(fd);
    tune_listen_socket(fd);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { fr_sock_close(fd); return -1; }
    if (listen(fd, 8192) < 0) { fr_sock_close(fd); return -1; }
    return (int64_t)fd;
}

int64_t fr_tcp_listen(int64_t port) {
    return tcp_listen_common(port, 0);
}

int64_t fr_tcp_listen_reuseport(int64_t port) {
    return tcp_listen_common(port, 1);
}

int64_t fr_tcp_accept(int64_t sock) {
    if (sock < 0) return -1;
    fr_socket_t client = accept((fr_socket_t)sock, NULL, NULL);
    if (client != FR_SOCK_INVALID) fr_sock_set_tcp_nodelay(client);
    return (int64_t)client;
}

int64_t fr_tcp_connect(const char *host, int64_t port) {
    if (!host) return -1;
    fr_platform_init();
    fr_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == FR_SOCK_INVALID) return -1;
    fr_sock_set_tcp_nodelay(fd);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fr_sock_close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fr_sock_close(fd);
        return -1;
    }
    return (int64_t)fd;
}

int64_t fr_tcp_send(int64_t sock, const char *data) {
    if (sock < 0 || !data) return -1;
    size_t total = strlen(data);
    size_t sent = 0;
    /* Loop rather than trusting one send(): a partial write is normal once the
     * payload exceeds the socket buffer, and returning early would silently
     * truncate the caller's message (HTTP request heads and bodies go out
     * through here). */
    while (sent < total) {
        ssize_t n = fr_sock_send((int)sock, data + sent, total - sent);
        if (n < 0) {
#if !defined(FORGE_OS_WINDOWS)
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return (int64_t)sent;
}

/* Reads until the peer half-closes, so the total length is chosen by the
 * remote end. Without a ceiling a single client that streams and never closes
 * grows this buffer until the process dies -- one socket, no authentication,
 * no special payload. Cap the total and fail the read instead: this API hands
 * back a NUL-terminated string and is not a bulk-transfer path, so anything
 * approaching the cap is a caller using the wrong tool or an attacker. */
#define FR_TCP_RECV_MAX ((size_t)8 * 1024 * 1024)

char *fr_tcp_recv(int64_t sock) {
    if (sock < 0) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= FR_TCP_RECV_MAX) { free(buf); return NULL; }
            size_t ncap = cap * 2;
            if (ncap > FR_TCP_RECV_MAX) ncap = FR_TCP_RECV_MAX;
            char *nbuf = (char *)realloc(buf, ncap);
            if (!nbuf) { free(buf); return NULL; }
            buf = nbuf;
            cap = ncap;
        }
        ssize_t n = fr_sock_recv(sock, buf + len, cap - len - 1);
        if (n < 0) { free(buf); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    return buf;
}

void fr_tcp_close(int64_t sock) {
    if (sock >= 0) fr_sock_close(sock);
}
