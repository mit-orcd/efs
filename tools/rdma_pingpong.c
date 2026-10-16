/* RDMA request/reply latency between two efs_conn endpoints in one process
 * (real verbs device; not part of `make test`). The server thread waits
 * exactly as server_handle_conn does (efs_conn_wait_request, then
 * efs_rdma_message_wait), the client as an RPC caller (efs_conn_send_msg,
 * efs_conn_recv_msg). Reports the round-trip distribution; a wake that
 * only lands on a poll timeout shows up as ~1-2 ms steps.
 * SIZE is the request payload (multi-segment above one physical buffer);
 * BURST requests are sent before their replies are read.
 *   cc -O2 -std=gnu11 -D_GNU_SOURCE -Iinclude -Isrc/common tools/rdma_pingpong.c \
 *      libefs.a -lpthread -lm -ldl -libverbs -o rdma_pingpong
 *   EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 ./rdma_pingpong ADDR PORT [N] [GAP_US] [SIZE] [BURST] */
#include "efs/common.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int lfd;
static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void *server(void *arg)
{
    (void)arg;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return NULL;
    struct efs_conn *c = efs_conn_wrap_tcp(fd, 1);
    for (;;) {
        int chan = efs_conn_wait_request(c);
        if (chan < 0) break;
        c->recv_chan = chan;
        if (chan == EFS_CONN_TCP) {
            uint8_t type; void *p = NULL; uint32_t n = 0;
            if (efs_recv_msg(fd, &type, &p, &n)) break;
            if (type == EFS_MSG_RDMA_SETUP) {
                uint8_t rep[EFS_RMSG_SETUP_BYTES]; uint32_t rl = sizeof(rep);
                efs_rdma_server_accept(c, p, n, rep, &rl);
                if (efs_send_msg(fd, EFS_MSG_RDMA_SETUP_REPLY, rep, rl)) break;
            }
            free(p);
            continue;
        }
        if (efs_rdma_message_wait(c->rc, -1)) break;
        uint32_t flen = 0;
        uint8_t *frame = efs_rdma_message_frame(c->rc, &flen);
        if (!frame || flen < 5) break;
        uint8_t type = frame[4];
        /* Reply after release, as the handler does once its switch ends. */
        uint8_t body[48];
        uint32_t bl = flen - 5 < sizeof(body) ? flen - 5 : sizeof(body);
        memcpy(body, frame + 5, bl);
        efs_rdma_message_release(c->rc);
        if (efs_conn_send_msg(c, type, body, bl)) break;
    }
    efs_conn_destroy(c);
    return NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s ADDR PORT [N] [GAP_US]\n", argv[0]); return 2; }
    const char *addr = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    int n = argc > 3 ? atoi(argv[3]) : 2000;
    int gap = argc > 4 ? atoi(argv[4]) : 0;
    uint32_t size = argc > 5 ? (uint32_t)atoi(argv[5]) : 48;
    int burst = argc > 6 ? atoi(argv[6]) : 1;
    if (burst < 1) burst = 1;
    lfd = efs_listen_tcp(addr, port, 4);
    if (lfd < 0) { perror("listen"); return 1; }
    pthread_t t;
    pthread_create(&t, NULL, server, NULL);
    int fd = efs_connect_tcp(addr, port);
    struct efs_conn *c = efs_conn_wrap_tcp(fd, 0);
    if (!c || efs_rdma_client_upgrade(c) != 0 || !c->rc) {
        fprintf(stderr, "rdma upgrade failed (transport=%d)\n", efs_rdma_transport());
        return 1;
    }
    uint64_t *rtt = calloc((size_t)n, sizeof(*rtt));
    uint8_t *msg = calloc(1, size);
    for (int i = 0; i < n; i += burst) {
        if (gap) usleep((useconds_t)gap);
        uint64_t a = now_ns();
        int k = n - i < burst ? n - i : burst;
        for (int j = 0; j < k; j++)
            if (efs_conn_send_msg(c, EFS_MSG_RDMA_SETUP_REPLY, msg, size)) { fprintf(stderr, "send %d failed\n", i + j); return 1; }
        for (int j = 0; j < k; j++) {
            uint8_t type; void *p = NULL; uint32_t pl = 0;
            if (efs_conn_recv_msg(c, &type, &p, &pl)) { fprintf(stderr, "recv %d failed\n", i + j); return 1; }
            free(p);
            rtt[i + j] = now_ns() - a;
        }
    }
    qsort(rtt, (size_t)n, sizeof(*rtt), cmp_u64);
    unsigned b[6] = {0};
    for (int i = 0; i < n; i++) {
        uint64_t us = rtt[i] / 1000;
        b[us < 100 ? 0 : us < 500 ? 1 : us < 900 ? 2 : us < 1500 ? 3 : us < 2500 ? 4 : 5]++;
    }
    printf("size=%u burst=%d ", size, burst);
    printf("rtt n=%d gap_us=%d p10=%.1fus p50=%.1fus p90=%.1fus p99=%.1fus max=%.1fus | <100us=%u 100-500=%u 500-900=%u 0.9-1.5ms=%u 1.5-2.5ms=%u >2.5ms=%u\n",
           n, gap, rtt[n / 10] / 1e3, rtt[n / 2] / 1e3, rtt[n * 9 / 10] / 1e3,
           rtt[n * 99 / 100] / 1e3, rtt[n - 1] / 1e3, b[0], b[1], b[2], b[3], b[4], b[5]);
    efs_conn_destroy(c);
    return 0;
}
