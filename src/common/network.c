#include "efs/network.h"
#include "efs/common.h"
#include "efs/rdma.h"
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
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <execinfo.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>

/* Keep short: a missing peer must not stall small-file meta flushes for long.
 * Down-marked peers are skipped entirely for EFS_NODE_DOWN_MS after one fail. */
#define EFS_CONNECT_TIMEOUT_SEC 2

static __thread uint64_t net_deadline_ms;

void efs_net_set_deadline_ms(uint64_t deadline)
{
    net_deadline_ms = deadline;
}

uint64_t efs_net_deadline_ms(void)
{
    return net_deadline_ms;
}

int efs_net_remaining_ms(int cap_ms)
{
    if (!net_deadline_ms)
        return cap_ms;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (now >= net_deadline_ms)
        return 0;
    uint64_t remaining = net_deadline_ms - now;
    if (cap_ms >= 0 && remaining > (uint64_t)cap_ms)
        return cap_ms;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 +
                   EFS_CONNECT_TIMEOUT_SEC * 1000;
    if (efs_net_deadline_ms() && efs_net_deadline_ms() < end)
        end = efs_net_deadline_ms();
    if (!efs_net_remaining_ms(INT_MAX)) {
        errno = ETIMEDOUT;
        return -1;
    }
    int fd = socket(addr->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    efs_tcp_keepalive(fd);
    int buf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto failed;
    int rc = connect(fd, addr, addrlen);
    if (rc < 0 && errno != EINPROGRESS)
        goto failed;
    if (rc < 0) {
        /* poll supports descriptors above FD_SETSIZE. One deadline spans
         * interrupted waits, and is capped by the enclosing RPC. */
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now >= end) { errno = ETIMEDOUT; goto failed; }
            rc = poll(&pfd, 1, (int)(end - now));
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc <= 0) {
                if (!rc) errno = ETIMEDOUT;
                goto failed;
            }
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0)
                goto failed;
            if (so_error) { errno = so_error; goto failed; }
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if ((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 >= end) {
        errno = ETIMEDOUT;
        goto failed;
    }
    if (fcntl(fd, F_SETFL, flags) < 0)
        goto failed;
    return fd;
failed:;
    int saved = errno;
    close(fd);
    errno = saved;
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

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

int efs_recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        int bounded = net_deadline_ms != 0;
        if (bounded) {
            int remaining = efs_net_remaining_ms(INT_MAX);
            if (!remaining) {
                errno = ETIMEDOUT;
                return -1;
            }
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int ready = poll(&pfd, 1, remaining);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0) {
                if (!ready) errno = ETIMEDOUT;
                return -1;
            }
        }
        ssize_t n = recv(fd, p + got, len - got, bounded ? MSG_DONTWAIT : 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR ||
                (bounded && (errno == EAGAIN || errno == EWOULDBLOCK))))
                continue;
            if (n == 0)
                errno = ECONNRESET;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static void conn_capture_fd(struct efs_conn *c)
{
    c->fd_id_ok = 0;
    c->fd_dev = 0;
    c->fd_ino = 0;
    if (!c || c->fd < 0)
        return;
    int fl = fcntl(c->fd, F_GETFD, 0);
    if (fl >= 0)
        (void)fcntl(c->fd, F_SETFD, fl | FD_CLOEXEC);
    struct stat st;
    if (fstat(c->fd, &st) != 0)
        return;
    c->fd_dev = (uint64_t)st.st_dev;
    c->fd_ino = (uint64_t)st.st_ino;
    c->fd_id_ok = 1;
}

int efs_conn_fd_matches(const struct efs_conn *c)
{
    if (!c || c->fd < 0)
        return 0;
    if (c->use_gen) {
        uint64_t cur;

        if (!c->gen_live)
            return 0;
        cur = __atomic_load_n(c->gen_live, __ATOMIC_RELAXED);
        return cur == c->gen_seen;
    }
    if (!c->fd_id_ok)
        return 0;
    struct stat st;
    if (fstat(c->fd, &st) != 0)
        return 0;
    return (uint64_t)st.st_dev == c->fd_dev &&
           (uint64_t)st.st_ino == c->fd_ino;
}

void efs_conn_bind_gen(struct efs_conn *c, uint64_t *slot)
{
    uint64_t g;

    if (!c || !slot)
        return;
    g = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (g == 0) {
        __atomic_store_n(slot, 1, __ATOMIC_RELAXED);
        g = 1;
    }
    c->use_gen = 1;
    c->gen_live = slot;
    c->gen_seen = g;
}

void efs_conn_note_ok(struct efs_conn *c)
{
    struct timespec ts;

    if (!c)
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->last_ok_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct efs_conn *efs_conn_wrap_tcp(int fd, int is_server)
{
    struct efs_conn *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->kind = EFS_CONN_TCP;
    c->is_server = is_server;
    c->fd = fd;
    c->recv_chan = EFS_CONN_TCP;
    c->rc = NULL;
    conn_capture_fd(c);
    return c;
}

void efs_conn_set_recv_timeout(struct efs_conn *c, int ms)
{
    if (!c)
        return;
    c->recv_timeout_ms = ms > 0 ? ms : 0;
    if (c->fd >= 0)
        (void)efs_set_recv_timeout(c->fd, ms > 0 ? ms : EFS_IO_TIMEOUT_MS);
}

void efs_conn_destroy(struct efs_conn *c)
{
    if (!c)
        return;
    if (c->rc && getenv("EFS_RDMA_FIRST")) {
        /* Closing the fd makes the peer tear down its QP, after which any
         * send still riding this conn is dropped with no error on either
         * side. Name the caller so a destroy that races an in-flight RPC is
         * attributable. */
        void *bt[8];
        int nb = backtrace(bt, 8);
        fprintf(stderr, "rdma-first: conn destroy fd=%d qpn=%u match=%d\n",
                c->fd, efs_rdma_qpn(c->rc), efs_conn_fd_matches(c));
        /* fd 2, not fileno(stderr): the daemons' stderr is a cookie
         * stream (log_ts.c) whose fileno is -1. */
        fflush(stderr);
        backtrace_symbols_fd(bt, nb, 2);
    }
    int own = efs_conn_fd_matches(c);

    if (c->use_gen && c->gen_live)
        __atomic_fetch_add(c->gen_live, 1, __ATOMIC_RELAXED);
    if (c->rc)
        efs_rdma_conn_destroy(c->rc);
    /* Only close if this handle still owned the descriptor at entry.
     * The generation bump above makes a recycled number fail fd_matches
     * for anyone still holding the old pointer. */
    if (c->fd >= 0 && own)
        close(c->fd);
    c->fd = -1;
    free(c);
}
