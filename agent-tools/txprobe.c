/* txprobe: reproduce efsd's TCP send pattern and bisect what kills TSO.
 *
 *   txprobe server <port> <mode> <flat> <msgsz>
 *   txprobe client <ip> <port> <conns> <secs> <mode> <flat> <msgsz>
 *
 *   mode: s = stream (server sends continuously, client drains)
 *         r = request-reply (client sends 30B req, server replies msgsz)
 *   flat: 0 = writev(4B len, 1B type, payload) like efs_send_msg
 *         1 = single contiguous send
 *   msgsz: total wire bytes per message (efsd GET reply = 65574)
 *
 * Socket options mimic efs: TCP_NODELAY, 4 MiB SO_SNDBUF/SO_RCVBUF, blocking.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>

static int g_flat;
static int g_msgsz = 65574;
static int g_rr;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void setopts(int fd)
{
    int yes = 1, buf = 4 * 1024 * 1024;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
}

static int send_all(int fd, const void *b, size_t l)
{
    const char *p = b;
    while (l) {
        ssize_t n = send(fd, p, l, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n; l -= n;
    }
    return 0;
}

static int recv_all(int fd, void *b, size_t l)
{
    char *p = b;
    while (l) {
        ssize_t n = recv(fd, p, l, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        p += n; l -= n;
    }
    return 0;
}

static int send_msg(int fd, uint8_t *body)
{
    if (g_flat)
        return send_all(fd, body, g_msgsz);
    struct iovec v[3] = {
        { .iov_base = body,     .iov_len = 4 },
        { .iov_base = body + 4, .iov_len = 1 },
        { .iov_base = body + 5, .iov_len = (size_t)g_msgsz - 5 },
    };
    int i = 0;
    while (i < 3) {
        ssize_t n = writev(fd, &v[i], 3 - i);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        size_t left = n;
        while (i < 3 && left >= v[i].iov_len) { left -= v[i].iov_len; i++; }
        if (i < 3 && left) { v[i].iov_base = (char *)v[i].iov_base + left; v[i].iov_len -= left; }
    }
    return 0;
}

static void *srv_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    setopts(fd);
    uint8_t *body = malloc(g_msgsz);
    memset(body, 0x5a, g_msgsz);
    uint8_t req[64];
    for (;;) {
        if (g_rr && recv_all(fd, req, 30)) break;
        if (send_msg(fd, body)) break;
    }
    close(fd);
    free(body);
    return NULL;
}

static int run_server(const char *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(atoi(port)) };
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) || listen(lfd, 512)) {
        perror("bind/listen");
        return 1;
    }
    fprintf(stderr, "server listening on %s (mode=%s flat=%d msgsz=%d)\n",
            port, g_rr ? "rr" : "stream", g_flat, g_msgsz);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue;
        pthread_t t;
        pthread_create(&t, NULL, srv_conn, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
    return 0;
}

struct carg {
    const char *ip;
    int port, secs;
    uint64_t bytes;
};

static void *cli_conn(void *arg)
{
    struct carg *c = arg;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(c->port) };
    inet_pton(AF_INET, c->ip, &sa.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) { perror("connect"); return NULL; }
    setopts(fd);
    uint8_t *buf = malloc(g_msgsz);
    uint8_t req[30] = {0};
    double end = now_s() + c->secs;
    while (now_s() < end) {
        if (g_rr) {
            if (send_all(fd, req, sizeof(req)) || recv_all(fd, buf, g_msgsz)) break;
            c->bytes += g_msgsz;
        } else {
            ssize_t n = recv(fd, buf, g_msgsz, 0);
            if (n <= 0) break;
            c->bytes += n;
        }
    }
    close(fd);
    free(buf);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 1;
    if (!strcmp(argv[1], "server")) {
        /* server <port> <mode> <flat> <msgsz> */
        g_rr = argv[3][0] == 'r';
        g_flat = atoi(argv[4]);
        g_msgsz = atoi(argv[5]);
        return run_server(argv[2]);
    }
    /* client <ip> <port> <conns> <secs> <mode> <flat> <msgsz> */
    const char *ip = argv[2];
    int port = atoi(argv[3]), conns = atoi(argv[4]), secs = atoi(argv[5]);
    g_rr = argv[6][0] == 'r';
    g_flat = atoi(argv[7]);
    g_msgsz = atoi(argv[8]);
    pthread_t *t = calloc(conns, sizeof(*t));
    struct carg *c = calloc(conns, sizeof(*c));
    double t0 = now_s();
    for (int i = 0; i < conns; i++) {
        c[i].ip = ip; c[i].port = port; c[i].secs = secs;
        pthread_create(&t[i], NULL, cli_conn, &c[i]);
    }
    uint64_t total = 0;
    for (int i = 0; i < conns; i++) { pthread_join(t[i], NULL); total += c[i].bytes; }
    double dt = now_s() - t0;
    printf("conns=%d mode=%s flat=%d msgsz=%d: %.1f Gbps\n",
           conns, g_rr ? "rr" : "stream", g_flat, g_msgsz, total * 8.0 / dt / 1e9);
    return 0;
}
