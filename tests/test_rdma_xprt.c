/* RDMA transport smoke test: QP loopback over the local IB device.
 *
 * Skips (exit 0, "SKIP") when no usable IB device exists, so it is safe in
 * the generic `make test` run on non-IB nodes. On the efs test servers it
 * exercises: handshake, a 500ms quiet gap then a CREATE-sized frame,
 * inline/small/pool-max frames both ways, the TCP side-channel for
 * oversized frames, and teardown.
 *
 * Both endpoints live in this process on a socketpair; the server side
 * uses efs_conn_wait_request (same wait as handler.c).
 */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/rdma.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                    msg);                                             \
            exit(1);                                                  \
        }                                                             \
    } while (0)

struct server_ctx {
    struct efs_conn *conn;
    int saw_rdma;       /* requests that arrived over the QP */
    int saw_tcp_big;    /* oversized requests that arrived over TCP */
    int n;
};

/* Echo server: read one frame, reply with the same type+payload. Runs until
 * recv fails (client closed). Channel handling mirrors handler.c: the reply
 * follows the request's arrival channel via conn->recv_chan. */
static void *server_thread(void *arg)
{
    struct server_ctx *sc = arg;
    struct efs_conn *c = sc->conn;
    for (;;) {
        int chan = efs_conn_wait_request(c);
        if (chan < 0)
            return NULL;
        c->recv_chan = chan;

        uint8_t type = 0;
        void *payload = NULL;
        uint32_t plen = 0;
        if (efs_conn_recv_msg(c, &type, &payload, &plen) != 0)
            return NULL;

        sc->n++;
        if (chan == EFS_CONN_RDMA)
            sc->saw_rdma++;
        else
            sc->saw_tcp_big++;

        int src = efs_conn_send_msg(c, type, payload, plen);
        free(payload);
        if (src != 0)
            return NULL;
    }
}

struct upgrade_req {
    struct efs_conn *c;
    int rc;
};

static void *upgrade_thread(void *arg)
{
    struct upgrade_req *u = arg;
    u->rc = efs_rdma_client_upgrade(u->c);
    return NULL;
}

static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i * 131u);
}

int main(void)
{
    if (!efs_rdma_available()) {
        printf("SKIP: no usable IB device (EFS_TRANSPORT/EFS_RDMA_DEV)\n");
        return 0;
    }

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    struct efs_conn *cli = efs_conn_wrap_tcp(sv[0], 0);
    struct efs_conn *srv = efs_conn_wrap_tcp(sv[1], 1);
    CHECK(cli && srv, "wrap");

    /* Handshake: client upgrade runs in a thread (it blocks for the reply);
     * the server side is serviced inline on the raw fd. */
    struct upgrade_req u = { cli, -1 };
    pthread_t cth;
    CHECK(pthread_create(&cth, NULL, upgrade_thread, &u) == 0, "cthread");
    {
        /* If the upgrade fails before sending SETUP, fail fast instead of
         * hanging on the socketpair read. */
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t type = 0;
        void *pl = NULL;
        uint32_t plen = 0;
        if (efs_recv_msg(sv[1], &type, &pl, &plen) != 0) {
            pthread_join(cth, NULL);
            fprintf(stderr, "FAIL: no SETUP received (client upgrade rc=%d)\n",
                    u.rc);
            return 1;
        }
        CHECK(type == EFS_MSG_RDMA_SETUP, "SETUP type");
        struct efs_msg_rdma_setup_reply rep;
        uint32_t rlen = sizeof(rep);
        CHECK(efs_rdma_server_accept(srv, pl, plen, &rep, &rlen) == 0,
              "server_accept");
        free(pl);
        CHECK(efs_send_msg(sv[1], EFS_MSG_RDMA_SETUP_REPLY, &rep, rlen) == 0,
              "send SETUP_REPLY");
        tv.tv_sec = 0;
        setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    pthread_join(cth, NULL);
    CHECK(u.rc == 0, "client_upgrade");
    CHECK(cli->rc && srv->rc, "both ends RDMA");
    CHECK(efs_rdma_live_conns() == 2, "live conns == 2");
    CHECK(efs_rdma_max_frame(cli->rc) >= EFS_RDMA_BUFSZ, "max frame");

    struct server_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.conn = srv;
    pthread_t eth;
    CHECK(pthread_create(&eth, NULL, server_thread, &sc) == 0, "echo thread");

    /* Quiet gap then a CREATE-sized frame: FUSE upgrades, sits idle through
     * FUSE_INIT, then the first mkdir is the first RDMA control SEND.
     * Immediate post-upgrade frames do not catch a poller that drops the
     * next CQE after a quiet gap. */
    usleep(500000);
    {
        struct efs_msg_inode_create cr;
        memset(&cr, 0, sizeof(cr));
        cr.export_id = 1;
        cr.parent = 1;
        memcpy(cr.name, "rdma-first", 10);
        cr.mode = 0755;
        uint8_t *out = NULL;
        uint32_t olen = 0;
        uint8_t rt = 0;
        CHECK(sizeof(cr) + 5 > EFS_RDMA_INLINE_MAX, "CREATE exceeds INLINE");
        CHECK(efs_conn_send_msg(cli, EFS_MSG_INODE_CREATE, &cr, sizeof(cr)) == 0,
              "send CREATE-sized after quiet gap");
        CHECK(efs_conn_recv_msg(cli, &rt, (void **)&out, &olen) == 0,
              "recv CREATE-sized echo");
        CHECK(rt == EFS_MSG_INODE_CREATE && olen == sizeof(cr),
              "CREATE-sized echo hdr");
        CHECK(memcmp(&cr, out, olen) == 0, "CREATE-sized echo payload");
        free(out);
    }

    /* 1. Tiny frame (inline path). */
    {
        uint8_t in[16], *out = NULL;
        fill_pattern(in, sizeof(in), 3);
        uint32_t olen = 0;
        uint8_t rt = 0;
        CHECK(efs_conn_send_msg(cli, EFS_MSG_BENCH_PUT, in, sizeof(in)) == 0,
              "send tiny");
        CHECK(efs_conn_recv_msg(cli, &rt, (void **)&out, &olen) == 0,
              "recv tiny echo");
        CHECK(rt == EFS_MSG_BENCH_PUT && olen == sizeof(in), "tiny echo hdr");
        CHECK(memcmp(in, out, olen) == 0, "tiny echo payload");
        free(out);
    }

    /* 1b. Many small frames: catches the shared-CQ poller sleeping without
     * re-arming after a harvest (the second LOOKUP after upgrade hung). */
    for (int i = 0; i < 64; i++) {
        uint8_t in[16], *out = NULL;
        fill_pattern(in, sizeof(in), (uint8_t)(20 + i));
        uint32_t olen = 0;
        uint8_t rt = 0;
        CHECK(efs_conn_send_msg(cli, EFS_MSG_BENCH_PUT, in, sizeof(in)) == 0,
              "send tiny-n");
        CHECK(efs_conn_recv_msg(cli, &rt, (void **)&out, &olen) == 0,
              "recv tiny-n");
        CHECK(rt == EFS_MSG_BENCH_PUT && olen == sizeof(in) &&
              memcmp(in, out, olen) == 0, "tiny-n echo");
        free(out);
    }

    /* 2. Pool-max frame (registered-buffer path, 64 KiB fragment size). */
    {
        uint32_t len = EFS_RDMA_BUFSZ - 5;
        uint8_t *in = malloc(len), *out = NULL;
        CHECK(in, "malloc");
        fill_pattern(in, len, 7);
        uint32_t olen = 0;
        uint8_t rt = 0;
        CHECK(efs_conn_send_msg(cli, EFS_MSG_PUT_CHUNK, in, len) == 0,
              "send pool-max");
        CHECK(efs_conn_recv_msg(cli, &rt, (void **)&out, &olen) == 0,
              "recv pool-max echo");
        CHECK(rt == EFS_MSG_PUT_CHUNK && olen == len, "pool-max hdr");
        CHECK(memcmp(in, out, olen) == 0, "pool-max payload (DMA integrity)");
        free(in);
        free(out);
    }

    /* 3. Oversized frame: must ride the TCP side-channel. */
    {
        uint32_t len = EFS_RDMA_BUFSZ + 1000;
        uint8_t *in = malloc(len), *out = NULL;
        CHECK(in, "malloc big");
        fill_pattern(in, len, 11);
        uint32_t olen = 0;
        uint8_t rt = 0;
        CHECK(efs_conn_send_msg(cli, EFS_MSG_PUT_CHUNK, in, len) == 0,
              "send oversized");
        CHECK(efs_conn_recv_msg(cli, &rt, (void **)&out, &olen) == 0,
              "recv oversized echo");
        CHECK(rt == EFS_MSG_PUT_CHUNK && olen == len, "oversized hdr");
        CHECK(memcmp(in, out, olen) == 0, "oversized payload");
        free(in);
        free(out);
    }

    /* Let the echo thread observe everything, then tear down. */
    usleep(100000);
    efs_conn_destroy(cli);
    pthread_join(eth, NULL);
    efs_conn_destroy(srv);

    CHECK(sc.saw_rdma >= 2, "server saw RDMA frames");
    CHECK(sc.saw_tcp_big >= 1, "server saw oversized on TCP");
    CHECK(efs_rdma_live_conns() == 0, "live conns back to 0");

    printf("PASS: rdma xprt (rdma=%d tcp_big=%d frames=%d)\n",
           sc.saw_rdma, sc.saw_tcp_big, sc.n);
    return 0;
}
