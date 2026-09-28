/* Transport-only TCP / RDMA SEND echo.
 *
 * Not efsd, and not a filesystem. A completion here is the echo of the same
 * bytes. It is not an fsync and not a Raft commit.
 *
 * The transport is selected with --transport tcp|rdma and checked on every
 * send. "rdma" fails closed: a failed upgrade, or a send that the pool
 * places on the TCP side-channel, is upgrade_failed / rejected_fallback.
 * Those bytes are not reported as RDMA. Frames larger than the RDMA pool
 * (EFS_RDMA_BUFSZ, 72 KiB) are rejected before send. A 128 KiB chunk does
 * not fit; a 64 KiB fragment does (5-byte header + payload).
 *
 * Do not point this at port 19810. The process refuses that port. Setting
 * EFS_TRANSPORT here does not change efsd or efs-fuse.
 */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/rdma.h"
#include "efs/wire.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* nrecv_bufs() in src/common/rdma.c clamps EFS_RDMA_BUFS to this. One QP
 * cannot hold more posted receives than that. */
#define XPRT_NRECV_CAP 64
#define XPRT_RECV_TIMEOUT_MS 5000

enum xprt_mode {
    MODE_NONE = 0,
    MODE_LATENCY,
    MODE_THROUGHPUT,
    MODE_IDLE
};

struct xprt_opts {
    int is_server;
    int is_version;
    int self_check;
    char bind_addr[64];
    char host[64];
    uint16_t port;
    char transport[8];
    enum xprt_mode mode;
    uint32_t payload;
    int depth;
    int warmup;
    int samples;
    int idle_ms;
    int repeat;
    int clients;
    int client_index;
    int expect;
    int series_id;
    char phase[16];
    char stats_path[512];
    char ready_path[512];
};

struct totals {
    uint64_t bytes;
    uint64_t rdma_reqs;
    uint64_t tcp_reqs;
    uint64_t errors;
    uint32_t max_frame;
    int conns;
};

static struct totals g_tot;
static pthread_mutex_t g_tot_mu = PTHREAD_MUTEX_INITIALIZER;
static struct rusage g_cpu0;
static int g_cpu_started;
static pthread_mutex_t g_cpu_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static void fill_pat(uint8_t *b, uint32_t n, uint64_t seq)
{
    uint64_t s = seq;
    for (uint32_t i = 0; i < n; i += 8) {
        uint32_t m = n - i;
        if (m > 8)
            m = 8;
        memcpy(b + i, &s, m);
        s += 0x9E3779B97F4A7C15ull;
    }
}

static int check_pat(const uint8_t *b, uint32_t n, uint64_t seq)
{
    uint64_t s = seq;
    for (uint32_t i = 0; i < n; i += 8) {
        uint32_t m = n - i;
        if (m > 8)
            m = 8;
        uint64_t got = 0;
        uint64_t exp = s;
        memcpy(&got, b + i, m);
        if (m < 8) {
            uint64_t mask = (m == 0) ? 0 : ((1ull << (m * 8)) - 1ull);
            got &= mask;
            exp &= mask;
        }
        if (got != exp)
            return -1;
        s += 0x9E3779B97F4A7C15ull;
    }
    return 0;
}

/* Nearest rank. index = ceil(p/100 * n) - 1.
 * Snap values that land a hair off an integer: 99.9% of 5000 is 4995,
 * and 99.9 is not exact in binary. */
static int pct_index(int n, double p)
{
    if (n <= 1)
        return 0;
    double r = (p / 100.0) * (double)n;
    double nearest = round(r);
    if (fabs(r - nearest) < 1e-6)
        r = nearest;
    int idx = (int)ceil(r) - 1;
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    return idx;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double pct_us(uint64_t *v, int n, double p)
{
    if (n <= 0)
        return 0;
    qsort(v, (size_t)n, sizeof(uint64_t), cmp_u64);
    return (double)v[pct_index(n, p)] / 1000.0;
}

static void pin_rdma_bufs(void)
{
    const char *e = getenv("EFS_RDMA_BUFS");
    if (!e || e[0] == '\0')
        setenv("EFS_RDMA_BUFS", "64", 1);
}

static void pin_server_env(void)
{
    /* This process only. efsd is a different process. tcp mode disables
     * SETUP accept, so the echo server always offers RDMA. */
    setenv("EFS_TRANSPORT", "rdma", 1);
    pin_rdma_bufs();
}

static void pin_client_env(const char *transport)
{
    setenv("EFS_TRANSPORT", transport, 1);
    pin_rdma_bufs();
}

static int write_file(const char *path, const char *text)
{
    if (!path || !path[0])
        return 0;
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    if (fputs(text, f) < 0) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

static void note_cpu_start(void)
{
    pthread_mutex_lock(&g_cpu_mu);
    if (!g_cpu_started) {
        getrusage(RUSAGE_SELF, &g_cpu0);
        g_cpu_started = 1;
    }
    pthread_mutex_unlock(&g_cpu_mu);
}

static void tot_add(uint64_t bytes, int chan, int err, uint32_t max_frame)
{
    pthread_mutex_lock(&g_tot_mu);
    g_tot.bytes += bytes;
    if (chan == EFS_CONN_RDMA)
        g_tot.rdma_reqs++;
    else if (chan == EFS_CONN_TCP)
        g_tot.tcp_reqs++;
    if (err)
        g_tot.errors++;
    if (max_frame > g_tot.max_frame)
        g_tot.max_frame = max_frame;
    pthread_mutex_unlock(&g_tot_mu);
}

static int echo_payload(struct efs_conn *c, uint8_t type, void *payload,
                        uint32_t plen, int chan)
{
    note_cpu_start();
    uint32_t max_frame = c->rc ? efs_rdma_max_frame(c->rc) : 0;
    int src = efs_conn_send_msg(c, type, payload, plen);
    tot_add(src == 0 ? plen : 0, chan, src != 0, max_frame);
    return src;
}

static void *conn_thread(void *arg)
{
    struct efs_conn *c = arg;
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t plen = 0;

    if (efs_recv_msg(c->fd, &type, &payload, &plen) != 0) {
        tot_add(0, -1, 1, 0);
        efs_conn_destroy(c);
        return NULL;
    }
    if (type == EFS_MSG_RDMA_SETUP) {
        struct efs_msg_rdma_setup_reply rep;
        uint32_t rlen = sizeof(rep);
        int arc = efs_rdma_server_accept(c, payload, plen, &rep, &rlen);
        free(payload);
        payload = NULL;
        if (arc != 0 ||
            efs_send_msg(c->fd, EFS_MSG_RDMA_SETUP_REPLY, &rep, rlen) != 0 ||
            !c->rc) {
            tot_add(0, -1, 1, 0);
            efs_conn_destroy(c);
            return NULL;
        }
    } else {
        c->recv_chan = EFS_CONN_TCP;
        if (echo_payload(c, type, payload, plen, EFS_CONN_TCP) != 0) {
            free(payload);
            efs_conn_destroy(c);
            return NULL;
        }
        free(payload);
        payload = NULL;
    }

    for (;;) {
        int chan = efs_conn_wait_request(c);
        if (chan < 0)
            break;
        c->recv_chan = chan;
        type = 0;
        payload = NULL;
        plen = 0;
        if (efs_conn_recv_msg(c, &type, &payload, &plen) != 0)
            break;
        int src = echo_payload(c, type, payload, plen, chan);
        free(payload);
        if (src != 0)
            break;
    }
    pthread_mutex_lock(&g_tot_mu);
    g_tot.conns++;
    pthread_mutex_unlock(&g_tot_mu);
    efs_conn_destroy(c);
    return NULL;
}

static int run_server(const struct xprt_opts *o)
{
    pin_server_env();
    int lfd = efs_listen_tcp(o->bind_addr, o->port, o->expect + 8);
    if (lfd < 0) {
        fprintf(stderr, "xprt_bench: listen %s:%u failed\n", o->bind_addr,
                o->port);
        return 1;
    }
    if (write_file(o->ready_path, "ready\n") != 0) {
        fprintf(stderr, "xprt_bench: ready file %s: %s\n", o->ready_path,
                strerror(errno));
        close(lfd);
        return 1;
    }

    pthread_t *ths = calloc((size_t)o->expect, sizeof(*ths));
    if (!ths) {
        close(lfd);
        return 1;
    }
    int nacc = 0;
    for (int i = 0; i < o->expect; i++) {
        struct pollfd p = { .fd = lfd, .events = POLLIN };
        if (poll(&p, 1, 15000) <= 0) {
            fprintf(stderr, "xprt_bench: accept timeout after %d/%d\n", nacc,
                    o->expect);
            break;
        }
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            fprintf(stderr, "xprt_bench: accept: %s\n", strerror(errno));
            break;
        }
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        efs_tcp_keepalive(fd);
        efs_set_recv_timeout(fd, XPRT_RECV_TIMEOUT_MS);
        efs_set_send_timeout(fd, XPRT_RECV_TIMEOUT_MS);
        struct efs_conn *c = efs_conn_wrap_tcp(fd, 1);
        if (!c) {
            close(fd);
            break;
        }
        if (pthread_create(&ths[nacc], NULL, conn_thread, c) != 0) {
            efs_conn_destroy(c);
            break;
        }
        nacc++;
    }
    close(lfd);
    for (int i = 0; i < nacc; i++)
        pthread_join(ths[i], NULL);
    free(ths);

    struct rusage ru1;
    getrusage(RUSAGE_SELF, &ru1);
    double cpu = 0;
    if (g_cpu_started) {
        cpu = (double)(ru1.ru_utime.tv_sec - g_cpu0.ru_utime.tv_sec) +
              (double)(ru1.ru_stime.tv_sec - g_cpu0.ru_stime.tv_sec) +
              ((double)ru1.ru_utime.tv_usec - (double)g_cpu0.ru_utime.tv_usec +
               (double)ru1.ru_stime.tv_usec -
               (double)g_cpu0.ru_stime.tv_usec) /
                  1e6;
        if (cpu < 0)
            cpu = 0;
    }
    char line[640];
    snprintf(line, sizeof(line),
             "{\"role\":\"server\",\"bytes\":%" PRIu64
             ",\"rdma_reqs\":%" PRIu64 ",\"tcp_reqs\":%" PRIu64
             ",\"errors\":%" PRIu64 ",\"max_frame\":%u,\"conns\":%d,"
             "\"cpu_s\":%.6f,\"series\":%d,"
             "\"completion\":\"transport_echo\"}\n",
             g_tot.bytes, g_tot.rdma_reqs, g_tot.tcp_reqs, g_tot.errors,
             g_tot.max_frame, g_tot.conns, cpu, o->series_id);
    if (o->stats_path[0]) {
        if (write_file(o->stats_path, line) != 0) {
            fprintf(stderr, "xprt_bench: stats %s: %s\n", o->stats_path,
                    strerror(errno));
            return 1;
        }
    } else {
        fputs(line, stdout);
    }
    return nacc == o->expect ? 0 : 1;
}

struct series {
    const char *status;
    int samples_n;
    double median_us;
    double p99_us;
    double p999_us;
    uint64_t bytes;
    double elapsed_s;
    double gib_s;
    double client_cpu_s;
    double client_cpu_s_per_gib;
    int errors;
    int timeouts;
    int fallbacks;
    int mismatches;
    int verified;
    uint32_t max_frame;
    uint32_t frame_len;
    int inline_send;
    int rdma_sends;
    int tcp_sends;
    char observed[8];
    char hostname[64];
};

static void series_init(struct series *s)
{
    memset(s, 0, sizeof(*s));
    s->status = "error";
    gethostname(s->hostname, sizeof(s->hostname) - 1);
}

static void emit_client(const struct xprt_opts *o, const struct series *s)
{
    const char *mode = "latency";
    if (o->mode == MODE_THROUGHPUT)
        mode = "throughput";
    else if (o->mode == MODE_IDLE)
        mode = "idle";
    printf("{\"role\":\"client\",\"transport\":\"%s\",\"observed\":\"%s\","
           "\"mode\":\"%s\",\"phase\":\"%s\",\"payload\":%u,\"depth\":%d,"
           "\"clients\":%d,\"client_index\":%d,\"repeat\":%d,\"warmup\":%d,"
           "\"samples\":%d,\"idle_ms\":%d,\"median_us\":%.3f,\"p99_us\":%.3f,"
           "\"p999_us\":%.3f,\"bytes\":%" PRIu64 ",\"elapsed_s\":%.6f,"
           "\"gib_s\":%.6f,\"client_cpu_s\":%.6f,"
           "\"client_cpu_s_per_gib\":%.6f,\"errors\":%d,\"timeouts\":%d,"
           "\"fallbacks\":%d,\"mismatches\":%d,\"verified\":%s,"
           "\"status\":\"%s\",\"series\":%d,\"max_frame\":%u,\"frame_len\":%u,"
           "\"inline\":%s,\"rdma_sends\":%d,\"tcp_sends\":%d,"
           "\"build\":\"%s\",\"host\":\"%s\","
           "\"completion\":\"transport_echo\"}\n",
           o->transport, s->observed[0] ? s->observed : "none", mode, o->phase,
           o->payload, o->depth, o->clients, o->client_index, o->repeat,
           o->warmup, s->samples_n, o->idle_ms, s->median_us, s->p99_us,
           s->p999_us, s->bytes, s->elapsed_s, s->gib_s, s->client_cpu_s,
           s->client_cpu_s_per_gib, s->errors, s->timeouts, s->fallbacks,
           s->mismatches, s->verified ? "true" : "false", s->status, o->series_id,
           s->max_frame, s->frame_len, s->inline_send ? "true" : "false",
           s->rdma_sends, s->tcp_sends, EFS_BUILD_ID, s->hostname);
}

static int want_rdma(const struct xprt_opts *o)
{
    return strcmp(o->transport, "rdma") == 0;
}

static int send_checked(struct efs_conn *c, const struct xprt_opts *o,
                        const uint8_t *buf, uint32_t n, struct series *s)
{
    if (efs_conn_send_msg(c, EFS_MSG_BENCH_PUT, buf, n) != 0) {
        s->errors++;
        return -1;
    }
    int rdma = c->recv_chan == EFS_CONN_RDMA;
    if (rdma)
        s->rdma_sends++;
    else
        s->tcp_sends++;
    if (want_rdma(o) != rdma) {
        s->fallbacks++;
        return -2;
    }
    return 0;
}

static int recv_checked(struct efs_conn *c, uint32_t n, uint64_t seq,
                        struct series *s, uint64_t t0)
{
    uint8_t rt = 0;
    void *out = NULL;
    uint32_t olen = 0;
    int rc = efs_conn_recv_msg(c, &rt, &out, &olen);
    uint64_t dt = now_ns() - t0;
    if (rc != 0) {
        if (dt >= (uint64_t)XPRT_RECV_TIMEOUT_MS * 1000000ull ||
            errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
            s->timeouts++;
        else
            s->errors++;
        return -1;
    }
    if (rt != EFS_MSG_BENCH_PUT || olen != n || check_pat(out, n, seq) != 0) {
        s->mismatches++;
        free(out);
        return -1;
    }
    free(out);
    s->bytes += n;
    return 0;
}

static int one_exchange(struct efs_conn *c, const struct xprt_opts *o,
                        uint8_t *buf, uint32_t n, uint64_t seq, struct series *s,
                        uint64_t *dt_ns)
{
    fill_pat(buf, n, seq);
    uint64_t t0 = now_ns();
    int sc = send_checked(c, o, buf, n, s);
    if (sc != 0)
        return sc;
    if (recv_checked(c, n, seq, s, t0) != 0)
        return -1;
    if (dt_ns)
        *dt_ns = now_ns() - t0;
    return 0;
}

static int pipeline(struct efs_conn *c, const struct xprt_opts *o, uint8_t *buf,
                    uint32_t n, uint64_t seq, struct series *s, uint64_t *dt_ns)
{
    uint64_t t0 = now_ns();
    int posted = 0;
    for (int i = 0; i < o->depth; i++) {
        fill_pat(buf, n, seq + (uint64_t)i);
        int sc = send_checked(c, o, buf, n, s);
        if (sc != 0)
            break;
        posted++;
    }
    int bad = 0;
    for (int i = 0; i < posted; i++) {
        if (recv_checked(c, n, seq + (uint64_t)i, s, t0) != 0) {
            bad = 1;
            break;
        }
    }
    if (posted != o->depth || bad)
        return -1;
    if (dt_ns)
        *dt_ns = now_ns() - t0;
    return 0;
}

static void finish_status(struct series *s)
{
    if (s->rdma_sends && !s->tcp_sends)
        snprintf(s->observed, sizeof(s->observed), "rdma");
    else if (s->tcp_sends && !s->rdma_sends)
        snprintf(s->observed, sizeof(s->observed), "tcp");
    else if (s->rdma_sends && s->tcp_sends)
        snprintf(s->observed, sizeof(s->observed), "mixed");
    else
        snprintf(s->observed, sizeof(s->observed), "none");

    if (strcmp(s->status, "rejected_oversize") == 0 ||
        strcmp(s->status, "upgrade_failed") == 0 ||
        strcmp(s->status, "rejected_qp_depth") == 0 ||
        strcmp(s->status, "connect_failed") == 0)
        return;
    if (s->fallbacks)
        s->status = "rejected_fallback";
    else if (s->mismatches)
        s->status = "data_mismatch";
    else if (s->timeouts)
        s->status = "timeout";
    else if (s->errors)
        s->status = "error";
    else if (s->samples_n <= 0)
        s->status = "error";
    else {
        s->status = "ok";
        s->verified = 1;
    }
}

static int status_exit(const struct series *s)
{
    if (strcmp(s->status, "ok") == 0 ||
        strcmp(s->status, "rejected_oversize") == 0)
        return 0;
    return 3;
}

static int run_client(struct xprt_opts *o)
{
    struct series s;
    series_init(&s);
    pin_client_env(o->transport);

    if (o->depth < 1 || o->depth > XPRT_NRECV_CAP) {
        s.status = "rejected_qp_depth";
        emit_client(o, &s);
        return status_exit(&s);
    }
    if ((o->mode == MODE_LATENCY || o->mode == MODE_IDLE) && o->depth != 1) {
        s.status = "bad_args";
        emit_client(o, &s);
        return status_exit(&s);
    }

    int fd = efs_connect_tcp(o->host, o->port);
    if (fd < 0) {
        s.status = "connect_failed";
        emit_client(o, &s);
        return status_exit(&s);
    }
    efs_set_recv_timeout(fd, XPRT_RECV_TIMEOUT_MS);
    efs_set_send_timeout(fd, XPRT_RECV_TIMEOUT_MS);
    struct efs_conn *c = efs_conn_wrap_tcp(fd, 0);
    if (!c) {
        close(fd);
        s.status = "connect_failed";
        emit_client(o, &s);
        return status_exit(&s);
    }

    if (want_rdma(o)) {
        if (efs_rdma_client_upgrade(c) != 0 || !c->rc ||
            c->kind != EFS_CONN_RDMA) {
            s.status = "upgrade_failed";
            emit_client(o, &s);
            efs_conn_destroy(c);
            return status_exit(&s);
        }
        s.max_frame = efs_rdma_max_frame(c->rc);
    }

    uint32_t frame = 0;
    if (efs_wire_frame_size(o->payload, &frame) != EFS_OK) {
        s.status = "error";
        emit_client(o, &s);
        efs_conn_destroy(c);
        return status_exit(&s);
    }
    s.frame_len = frame;
    s.inline_send = frame <= (uint32_t)EFS_RDMA_INLINE_MAX;
    if (want_rdma(o) && frame > s.max_frame) {
        /* Do not send. conn_pick_send_chan would silently use TCP. */
        s.status = "rejected_oversize";
        emit_client(o, &s);
        efs_conn_destroy(c);
        return status_exit(&s);
    }

    int warmup = o->warmup;
    int exchanges = o->samples;
    if (warmup < 0)
        warmup = (o->mode == MODE_LATENCY) ? 200 : (o->mode == MODE_IDLE) ? 2 : 1;
    if (o->mode == MODE_THROUGHPUT) {
        /* --samples is an explicit exchange count. The 256 MiB floor
         * applies only when the caller left the count unset. */
        if (exchanges < 0) {
            uint64_t target = 256ull << 20;
            exchanges = (int)(target / (o->payload ? o->payload : 1));
            if (exchanges < 256)
                exchanges = 256;
            if (exchanges > 100000)
                exchanges = 100000;
        }
        if (exchanges < o->depth)
            exchanges = o->depth;
        exchanges = (exchanges + o->depth - 1) / o->depth * o->depth;
    } else if (exchanges < 0) {
        exchanges = (o->mode == MODE_LATENCY) ? 5000 : 40;
    }
    o->warmup = warmup;

    uint8_t *buf = malloc(o->payload);
    int sample_cap = (o->mode == MODE_THROUGHPUT) ? (exchanges / o->depth) : exchanges;
    if (sample_cap < 1)
        sample_cap = 1;
    uint64_t *samples = calloc((size_t)sample_cap, sizeof(uint64_t));
    if (!buf || !samples) {
        free(buf);
        free(samples);
        s.status = "error";
        emit_client(o, &s);
        efs_conn_destroy(c);
        return status_exit(&s);
    }

    uint64_t seq = 1;
    for (int i = 0; i < warmup; i++) {
        int rc;
        if (o->mode == MODE_THROUGHPUT)
            rc = pipeline(c, o, buf, o->payload, seq, &s, NULL);
        else {
            if (o->mode == MODE_IDLE && o->idle_ms > 0)
                sleep_ms(o->idle_ms);
            rc = one_exchange(c, o, buf, o->payload, seq, &s, NULL);
        }
        seq += (o->mode == MODE_THROUGHPUT) ? (uint64_t)o->depth : 1;
        if (rc != 0)
            break;
    }
    /* Warmup bytes are not the measurement. */
    s.bytes = 0;

    double cpu0 = 0;
    uint64_t t0 = 0;
    int measured_ok = (s.fallbacks == 0 && s.errors == 0 && s.timeouts == 0 &&
                       s.mismatches == 0);
    if (measured_ok) {
        cpu0 = 0;
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        cpu0 = (double)ru.ru_utime.tv_sec + (double)ru.ru_stime.tv_sec +
               ((double)ru.ru_utime.tv_usec + (double)ru.ru_stime.tv_usec) / 1e6;
        t0 = now_ns();
        if (o->mode == MODE_THROUGHPUT) {
            int batches = exchanges / o->depth;
            for (int b = 0; b < batches; b++) {
                uint64_t dt = 0;
                if (pipeline(c, o, buf, o->payload, seq, &s, &dt) != 0)
                    break;
                if (s.samples_n < sample_cap)
                    samples[s.samples_n++] = dt / (uint64_t)o->depth;
                seq += (uint64_t)o->depth;
            }
        } else {
            for (int i = 0; i < exchanges; i++) {
                if (o->mode == MODE_IDLE && o->idle_ms > 0)
                    sleep_ms(o->idle_ms);
                uint64_t dt = 0;
                if (one_exchange(c, o, buf, o->payload, seq, &s, &dt) != 0)
                    break;
                /* Idle sleep is outside the sample. one_exchange times
                 * send+recv only, and it starts after the sleep. */
                if (s.samples_n < sample_cap)
                    samples[s.samples_n++] = dt;
                seq++;
            }
        }
        struct rusage ru1;
        getrusage(RUSAGE_SELF, &ru1);
        double cpu1 = (double)ru1.ru_utime.tv_sec + (double)ru1.ru_stime.tv_sec +
                      ((double)ru1.ru_utime.tv_usec +
                        (double)ru1.ru_stime.tv_usec) /
                          1e6;
        s.client_cpu_s = cpu1 - cpu0;
        if (s.client_cpu_s < 0)
            s.client_cpu_s = 0;
        if (o->mode == MODE_IDLE) {
            uint64_t sum = 0;
            for (int i = 0; i < s.samples_n; i++)
                sum += samples[i];
            s.elapsed_s = (double)sum / 1e9;
        } else {
            s.elapsed_s = (double)(now_ns() - t0) / 1e9;
        }
    }

    if (s.samples_n > 0) {
        s.median_us = pct_us(samples, s.samples_n, 50);
        s.p99_us = pct_us(samples, s.samples_n, 99);
        s.p999_us = pct_us(samples, s.samples_n, 99.9);
    }
    if (s.elapsed_s > 0 && s.bytes > 0) {
        double gib = (double)s.bytes / (1024.0 * 1024.0 * 1024.0);
        s.gib_s = gib / s.elapsed_s;
        if (gib > 0)
            s.client_cpu_s_per_gib = s.client_cpu_s / gib;
    }
    finish_status(&s);
    emit_client(o, &s);
    free(samples);
    free(buf);
    efs_conn_destroy(c);
    return status_exit(&s);
}

static int self_check(void)
{
    uint8_t buf[64];
    fill_pat(buf, sizeof(buf), 7);
    if (check_pat(buf, sizeof(buf), 7) != 0)
        return 1;
    buf[10] ^= 1;
    if (check_pat(buf, sizeof(buf), 7) == 0)
        return 1;
    if (pct_index(100, 50) != 49)
        return 1;
    if (pct_index(5000, 99.9) != 4994)
        return 1;
    uint64_t v[100];
    for (int i = 0; i < 100; i++)
        v[i] = (uint64_t)i * 1000ull;
    double med = pct_us(v, 100, 50);
    if (med < 48.9 || med > 49.1)
        return 1;
    uint32_t frame = 0;
    if (efs_wire_frame_size(131072, &frame) != EFS_OK || frame <= EFS_RDMA_BUFSZ)
        return 1;
    if (efs_wire_frame_size(65536, &frame) != EFS_OK || frame > EFS_RDMA_BUFSZ)
        return 1;
    if (efs_wire_frame_size(64, &frame) != EFS_OK ||
        frame > (uint32_t)EFS_RDMA_INLINE_MAX)
        return 1;
    printf("{\"role\":\"self-check\",\"status\":\"ok\","
           "\"chunk_frame_fits_rdma\":false,\"fragment_frame_fits_rdma\":true,"
           "\"completion\":\"transport_echo\"}\n");
    return 0;
}

static void print_version(void)
{
    printf("{\"role\":\"version\",\"build\":\"%s\",\"rdma_bufsz\":%d,"
           "\"inline_max\":%d,\"nrecv_cap\":%d,\"chunk\":%d,\"fragment\":%d,"
           "\"completion\":\"transport_echo\"}\n",
           EFS_BUILD_ID, EFS_RDMA_BUFSZ, EFS_RDMA_INLINE_MAX, XPRT_NRECV_CAP,
           EFS_CHUNK_SIZE, EFS_FRAGMENT_SIZE);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s version\n"
            "  %s self-check\n"
            "  %s server --bind ADDR --port N --expect N --ready F --stats F\n"
            "  %s client --host H --port N --transport tcp|rdma\n"
            "            --mode latency|throughput|idle --payload BYTES\n"
            "            [--depth N] [--warmup N] [--samples N] [--idle-ms N]\n"
            "            [--repeat N] [--clients N] [--client-index N]\n"
            "            [--phase warmup|measure]\n"
            "\n"
            "Echoes EFS_MSG_BENCH_PUT over the selected transport. Not efsd.\n"
            "Refuses ports 19810 and 19820. A 128 KiB payload on RDMA is\n"
            "rejected_oversize (it would fall onto the TCP side-channel).\n",
            argv0, argv0, argv0, argv0);
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s[0] || (end && *end) || v > 0xfffffffful)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int refused_port(uint16_t port)
{
    return port == 19810 || port == 19820 || port == 0;
}

static int parse_args(int argc, char **argv, struct xprt_opts *o)
{
    memset(o, 0, sizeof(*o));
    snprintf(o->bind_addr, sizeof(o->bind_addr), "0.0.0.0");
    snprintf(o->host, sizeof(o->host), "127.0.0.1");
    snprintf(o->phase, sizeof(o->phase), "measure");
    o->port = 19960;
    o->depth = 1;
    o->clients = 1;
    o->expect = 1;
    o->warmup = -1;
    o->samples = -1;
    if (argc < 2) {
        usage(argv[0]);
        return -1;
    }
    if (strcmp(argv[1], "version") == 0) {
        o->is_version = 1;
        return 0;
    }
    if (strcmp(argv[1], "self-check") == 0) {
        o->self_check = 1;
        return 0;
    }
    if (strcmp(argv[1], "server") == 0)
        o->is_server = 1;
    else if (strcmp(argv[1], "client") == 0)
        o->is_server = 0;
    else {
        usage(argv[0]);
        return -1;
    }
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--bind") == 0 && v) {
            snprintf(o->bind_addr, sizeof(o->bind_addr), "%s", v);
            i++;
        } else if (strcmp(a, "--host") == 0 && v) {
            snprintf(o->host, sizeof(o->host), "%s", v);
            i++;
        } else if (strcmp(a, "--port") == 0 && v) {
            uint32_t p = 0;
            if (parse_u32(v, &p) != 0 || p > 65535) {
                fprintf(stderr, "bad --port\n");
                return -1;
            }
            o->port = (uint16_t)p;
            i++;
        } else if (strcmp(a, "--transport") == 0 && v) {
            if (strcmp(v, "tcp") != 0 && strcmp(v, "rdma") != 0) {
                fprintf(stderr, "transport must be tcp or rdma\n");
                return -1;
            }
            snprintf(o->transport, sizeof(o->transport), "%s", v);
            i++;
        } else if (strcmp(a, "--mode") == 0 && v) {
            if (strcmp(v, "latency") == 0)
                o->mode = MODE_LATENCY;
            else if (strcmp(v, "throughput") == 0)
                o->mode = MODE_THROUGHPUT;
            else if (strcmp(v, "idle") == 0)
                o->mode = MODE_IDLE;
            else {
                fprintf(stderr, "bad --mode\n");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--payload") == 0 && v) {
            if (parse_u32(v, &o->payload) != 0 || o->payload == 0 ||
                o->payload > (1024u * 1024u)) {
                fprintf(stderr, "bad --payload\n");
                return -1;
            }
            i++;
        } else if (strcmp(a, "--depth") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0 || d == 0 || d > 100000) {
                fprintf(stderr, "bad --depth\n");
                return -1;
            }
            o->depth = (int)d;
            i++;
        } else if (strcmp(a, "--warmup") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0) {
                fprintf(stderr, "bad --warmup\n");
                return -1;
            }
            o->warmup = (int)d;
            i++;
        } else if (strcmp(a, "--samples") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0 || d > 200000) {
                fprintf(stderr, "bad --samples\n");
                return -1;
            }
            o->samples = (int)d;
            i++;
        } else if (strcmp(a, "--idle-ms") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0 || d > 60000) {
                fprintf(stderr, "bad --idle-ms\n");
                return -1;
            }
            o->idle_ms = (int)d;
            i++;
        } else if (strcmp(a, "--repeat") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0) {
                fprintf(stderr, "bad --repeat\n");
                return -1;
            }
            o->repeat = (int)d;
            i++;
        } else if (strcmp(a, "--clients") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0 || d == 0) {
                fprintf(stderr, "bad --clients\n");
                return -1;
            }
            o->clients = (int)d;
            i++;
        } else if (strcmp(a, "--client-index") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0) {
                fprintf(stderr, "bad --client-index\n");
                return -1;
            }
            o->client_index = (int)d;
            i++;
        } else if (strcmp(a, "--expect") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0 || d == 0 || d > 256) {
                fprintf(stderr, "bad --expect\n");
                return -1;
            }
            o->expect = (int)d;
            i++;
        } else if (strcmp(a, "--phase") == 0 && v) {
            if (strcmp(v, "warmup") != 0 && strcmp(v, "measure") != 0) {
                fprintf(stderr, "phase must be warmup or measure\n");
                return -1;
            }
            snprintf(o->phase, sizeof(o->phase), "%s", v);
            i++;
        } else if (strcmp(a, "--stats") == 0 && v) {
            snprintf(o->stats_path, sizeof(o->stats_path), "%s", v);
            i++;
        } else if (strcmp(a, "--ready") == 0 && v) {
            snprintf(o->ready_path, sizeof(o->ready_path), "%s", v);
            i++;
        } else if (strcmp(a, "--series") == 0 && v) {
            uint32_t d = 0;
            if (parse_u32(v, &d) != 0) {
                fprintf(stderr, "bad --series\n");
                return -1;
            }
            o->series_id = (int)d;
            i++;
        } else {
            fprintf(stderr, "unknown argument %s\n", a);
            return -1;
        }
    }
    if (refused_port(o->port)) {
        fprintf(stderr, "refusing port %u (live cluster is not this bench)\n",
                o->port);
        return -1;
    }
    if (!o->is_server) {
        if (o->transport[0] == '\0' || o->mode == MODE_NONE || o->payload == 0) {
            fprintf(stderr, "client needs --transport, --mode, and --payload\n");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct xprt_opts o;
    if (parse_args(argc, argv, &o) != 0)
        return 2;
    if (o.is_version) {
        print_version();
        return 0;
    }
    if (o.self_check)
        return self_check();
    if (o.is_server)
        return run_server(&o);
    return run_client(&o);
}
