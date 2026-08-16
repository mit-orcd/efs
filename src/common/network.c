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

/* Keep short: a missing peer must not stall small-file meta flushes for long.
 * Down-marked peers are skipped entirely for EFS_NODE_DOWN_MS after one fail. */
#define EFS_CONNECT_TIMEOUT_SEC 2

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
    efs_tcp_keepalive(fd);
    /* Large buffers help IB/IPoIB bulk fragment PUT streams. */
    int buf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

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
    if (!host || !*host)
        return -1;

    /* Numeric IPv4 only on the hot path. Engaging's NSS corrupts under
     * concurrent getaddrinfo (heartbeat SEGV in freeaddrinfo / ai_next).
     * Cluster membership should advertise IB IPs; hostnames are resolved
     * once under a lock below for rare non-numeric cases. */
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

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* IPv4-only; matches Engaging IB usage */
    hints.ai_socktype = SOCK_STREAM;

    static pthread_mutex_t gai_mu = PTHREAD_MUTEX_INITIALIZER;
    struct sockaddr_in sa;
    int have = 0;

    pthread_mutex_lock(&gai_mu);
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc == 0 && res && res->ai_addr &&
        res->ai_addrlen >= sizeof(struct sockaddr_in) &&
        res->ai_addr->sa_family == AF_INET) {
        memcpy(&sa, res->ai_addr, sizeof(sa));
        have = 1;
    }
    if (res)
        freeaddrinfo(res);
    pthread_mutex_unlock(&gai_mu);

    if (!have) {
        fprintf(stderr, "efs_connect_tcp: cannot resolve '%s'\n", host);
        return -1;
    }
    return connect_sockaddr((struct sockaddr *)&sa, sizeof(sa));
}

int efs_listen_tcp(const char *host, uint16_t port, int backlog)
{
    efs_net_init();

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (!host || !*host || strcmp(host, "0.0.0.0") == 0) {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "efs_listen_tcp: need numeric IPv4 addr, got '%s'\n",
                host ? host : "(null)");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int buf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "bind failed for %s:%u: %s\n", host ? host : "*", port,
                strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) != 0) {
        fprintf(stderr, "listen failed for %s:%u: %s\n", host ? host : "*", port,
                strerror(errno));
        close(fd);
        return -1;
    }
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

int efs_tcp_keepalive(int fd)
{
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) != 0)
        return -1;
#ifdef TCP_KEEPIDLE
    int idle = 30;
    int intvl = 10;
    int cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    return 0;
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
            if (n == 0)
                errno = ECONNRESET;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}
