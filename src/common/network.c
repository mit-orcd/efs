#include "efs/network.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/select.h>
#include <signal.h>
#include <pthread.h>

#define EFS_CONNECT_TIMEOUT_SEC 5

static void efs_ignore_sigpipe_once(void)
{
    /* Writing to a peer that has gone away must return EPIPE to the caller,
     * not kill the process. Network servers and FUSE clients both need this. */
    signal(SIGPIPE, SIG_IGN);
}

static void efs_net_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, efs_ignore_sigpipe_once);
}

static int connect_sockaddr(const struct sockaddr *addr, socklen_t addrlen)
{
    int fd = socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    int rc = connect(fd, addr, addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {EFS_CONNECT_TIMEOUT_SEC, 0};
        rc = select(fd + 1, NULL, &fds, NULL, &tv);
        if (rc > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 &&
                so_error == 0) {
                fcntl(fd, F_SETFL, flags);
                return fd;
            }
        }
    } else if (rc == 0) {
        fcntl(fd, F_SETFL, flags);
        return fd;
    }

    close(fd);
    return -1;
}

int efs_connect_tcp(const char *host, uint16_t port)
{
    efs_net_init();

    /* Fast path for numeric IPv4 (common for cluster peers / heartbeats):
     * skip getaddrinfo entirely. */
    struct in_addr addr4;
    if (inet_pton(AF_INET, host, &addr4) == 1) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr = addr4;
        return connect_sockaddr((struct sockaddr *)&sa, sizeof(sa));
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = connect_sockaddr(rp->ai_addr, rp->ai_addrlen);
        if (fd >= 0)
            break;
    }

    freeaddrinfo(res);
    return fd;
}

int efs_listen_tcp(const char *host, uint16_t port, int backlog)
{
    efs_net_init();

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (rp->ai_family == AF_INET6) {
            int no = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
            fprintf(stderr, "bind failed for %s:%u: %s\n", host, port, strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        if (listen(fd, backlog) != 0) {
            fprintf(stderr, "listen failed for %s:%u: %s\n", host, port, strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(res);
    return fd;
}

static int set_timeout(int fd, int ms, int opt)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
}

int efs_set_recv_timeout(int fd, int ms)
{
    return set_timeout(fd, ms, SO_RCVTIMEO);
}

int efs_set_send_timeout(int fd, int ms)
{
    return set_timeout(fd, ms, SO_SNDTIMEO);
}

int efs_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int efs_recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}
