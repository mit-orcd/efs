#include "efs/protocol.h"
#include "efs/wire.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int send_iov(int fd, struct iovec *iov, int niov)
{
    int i = 0;
    while (i < niov) {
        ssize_t n = writev(fd, &iov[i], niov - i);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_NET;
        }
        if (n == 0)
            return EFS_ERR_NET;
        size_t left = (size_t)n;
        while (i < niov && left >= iov[i].iov_len) {
            left -= iov[i].iov_len;
            i++;
        }
        if (i < niov && left > 0) {
            iov[i].iov_base = (uint8_t *)iov[i].iov_base + left;
            iov[i].iov_len -= left;
        }
    }
    return EFS_OK;
}

int efs_send_msg_parts(int fd, uint8_t type,
                       const void *part1, uint32_t part1_len,
                       const void *part2, uint32_t part2_len)
{
    uint32_t n1 = (part1_len > 0 && part1) ? part1_len : 0;
    uint32_t n2 = (part2_len > 0 && part2) ? part2_len : 0;
    uint8_t hdr[5];
    struct iovec iov[3];
    int niov = 1;
    int rc;
    if (n1 > UINT32_MAX - n2)
        return EFS_ERR_PROTO;
    rc = efs_wire_frame_header(type, n1 + n2, hdr);
    if (rc != EFS_OK)
        return rc;
    iov[0].iov_base = hdr;
    iov[0].iov_len = 5;
    if (n1) {
        iov[niov].iov_base = (void *)part1;
        iov[niov].iov_len = n1;
        niov++;
    }
    if (n2) {
        iov[niov].iov_base = (void *)part2;
        iov[niov].iov_len = n2;
        niov++;
    }
    return send_iov(fd, iov, niov);
}

int efs_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len)
{
    return efs_send_msg_parts(fd, type, payload, payload_len, NULL, 0);
}

int efs_recv_u8_reply(int fd, uint8_t *type, uint8_t *status)
{
    uint32_t be;
    uint32_t nlen;
    if (efs_recv_all(fd, &be, sizeof(be)) != 0)
        return EFS_ERR_NET;
    nlen = ntohl(be);
    /* nlen 2 is [type][status]. nlen 3 is a PUT reply with a path byte,
     * which a status-only reader ignores. */
    if (efs_wire_frame_check_nlen(nlen) != EFS_OK || nlen < 2 || nlen > 4)
        return EFS_ERR_PROTO;
    uint8_t raw[4];
    if (efs_recv_all(fd, raw, nlen) != 0)
        return EFS_ERR_NET;
    if (type)
        *type = raw[0];
    if (status)
        *status = raw[1];
    return EFS_OK;
}

int efs_recv_put_reply(int fd, uint8_t *type, uint8_t *status, uint8_t *path)
{
    uint32_t be;
    uint32_t nlen;
    if (path)
        *path = 0xff;
    if (efs_recv_all(fd, &be, sizeof(be)) != 0)
        return EFS_ERR_NET;
    nlen = ntohl(be);
    if (efs_wire_frame_check_nlen(nlen) != EFS_OK || nlen < 2 || nlen > 4)
        return EFS_ERR_PROTO;
    uint8_t raw[4];
    if (efs_recv_all(fd, raw, nlen) != 0)
        return EFS_ERR_NET;
    if (type)
        *type = raw[0];
    if (status)
        *status = raw[1];
    if (path && nlen >= 3)
        *path = raw[2];
    return EFS_OK;
}

int efs_recv_msg(int fd, uint8_t *type, void **payload, uint32_t *payload_len)
{
    uint32_t be;
    uint32_t nlen;
    uint8_t t;
    uint32_t plen;
    if (efs_recv_all(fd, &be, sizeof(be)) != 0)
        return EFS_ERR_NET;
    nlen = ntohl(be);
    if (efs_wire_frame_check_nlen(nlen) != EFS_OK)
        return EFS_ERR_PROTO;

    if (efs_recv_all(fd, &t, 1) != 0)
        return EFS_ERR_NET;
    *type = t;

    plen = nlen - 1;
    if (payload_len)
        *payload_len = plen;

    if (plen == 0) {
        if (payload)
            *payload = NULL;
        return EFS_OK;
    }

    if (!payload) {
        /* Drain unread payload. */
        uint8_t sink[4096];
        uint32_t left = plen;
        while (left) {
            uint32_t n = left > sizeof(sink) ? (uint32_t)sizeof(sink) : left;
            if (efs_recv_all(fd, sink, n) != 0)
                return EFS_ERR_NET;
            left -= n;
        }
        return EFS_OK;
    }

    /* Payload lands at malloc base — no 64 KiB memmove to strip the type. */
    uint8_t *buf = malloc(plen);
    if (!buf)
        return EFS_ERR_NOMEM;
    if (efs_recv_all(fd, buf, plen) != 0) {
        free(buf);
        return EFS_ERR_NET;
    }
    *payload = buf;
    return EFS_OK;
}

int efs_recv_msg_into(int fd, uint8_t *type, uint8_t *status,
                      void *hdr, uint32_t hdr_len, void *body, uint32_t body_len)
{
    uint32_t be;
    uint32_t nlen;
    uint8_t t;
    uint32_t plen;
    if (efs_recv_all(fd, &be, sizeof(be)) != 0)
        return EFS_ERR_NET;
    nlen = ntohl(be);
    if (efs_wire_frame_check_nlen(nlen) != EFS_OK)
        return EFS_ERR_PROTO;

    if (efs_recv_all(fd, &t, 1) != 0)
        return EFS_ERR_NET;
    *type = t;

    plen = nlen - 1;
    /* Read the status byte first. A status-only (error) reply is plen==1;
     * a full reply is plen == 1 + hdr_len + body_len. Anything else is a
     * protocol error. */
    if (plen != 1 && plen != 1 + hdr_len + body_len) {
        uint8_t sink[4096];
        uint32_t left = plen;
        while (left) {
            uint32_t n = left > sizeof(sink) ? (uint32_t)sizeof(sink) : left;
            if (efs_recv_all(fd, sink, n) != 0)
                return EFS_ERR_NET;
            left -= n;
        }
        return EFS_ERR_PROTO;
    }
    if (efs_recv_all(fd, status, 1) != 0)
        return EFS_ERR_NET;
    if (plen == 1)
        return EFS_OK;  /* status-only reply: hdr/body left untouched */
    if (hdr_len > 0 && efs_recv_all(fd, hdr, hdr_len) != 0)
        return EFS_ERR_NET;
    if (body_len > 0 && efs_recv_all(fd, body, body_len) != 0)
        return EFS_ERR_NET;
    return EFS_OK;
}

/* ---------------- struct efs_conn dispatchers (TCP / RDMA) ---------------- */

/* Channel rule (both ends compute it identically):
 *  - server: the reply follows the request's arrival channel, unless the
 *    reply frame exceeds the RDMA pool buffer (then TCP).
 *  - client: RDMA when the frame fits. */
static int conn_pick_send_chan(struct efs_conn *c, uint32_t frame_len)
{
    if (!c->rc)
        return EFS_CONN_TCP;
    if (frame_len > efs_rdma_max_frame(c->rc))
        return EFS_CONN_TCP;
    if (c->is_server)
        return c->recv_chan == EFS_CONN_RDMA ? EFS_CONN_RDMA : EFS_CONN_TCP;
    return EFS_CONN_RDMA;
}

/* True when the TCP fd has a real byte (or peer close). Spurious POLLIN
 * after RDMA upgrade used to send us into efs_recv_all with SO_RCVTIMEO=0,
 * which blocked forever while the CREATE sat on the RDMA ring. */
static int tcp_has_request(int fd)
{
    char peek;
    ssize_t n = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0)
        return 1;
    /* EOF and a socket error both tear the conn (and its QP) down, but they
     * have completely different causes: distinguish them. */
    if (n == 0) {
        if (getenv("EFS_RDMA_FIRST"))
            fprintf(stderr, "rdma-first: tcp EOF (peer FIN) fd=%d\n", fd);
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;
    if (getenv("EFS_RDMA_FIRST"))
        fprintf(stderr, "rdma-first: tcp recv error fd=%d errno=%d (%s)\n", fd,
                errno, strerror(errno));
    return -1;
}

/* Name why a conn is being torn down. Destroying the conn also destroys its
 * QP, and a peer still holding that QP gets NO error of any kind -- its
 * packets land on a QPN that no longer exists and are silently dropped, so
 * it retries until its own QP errors out with no completion. Without this
 * log the teardown is invisible from both ends. */
static int wait_teardown(struct efs_conn *c, const char *why)
{
    if (getenv("EFS_RDMA_FIRST"))
        fprintf(stderr, "rdma-first: conn teardown fd=%d qpn=%u reason=%s\n",
                c->fd, c->rc ? efs_rdma_qpn(c->rc) : 0, why);
    return -1;
}

int efs_conn_wait_request(struct efs_conn *c)
{
    if (!c->rc)
        return EFS_CONN_TCP;
    struct efs_rdma_conn *rc = c->rc;
    for (;;) {
        /* Quick check only: a server conn thread has nothing to gain from
         * spin-polling the ring — with dozens of live conns the aggregate
         * spin was ~13 cores/server under a 9-client write load. It blocks
         * on the eventfd below; the wakeup costs ~2us, nothing next to the
         * per-request work. The client's latency-critical reply path keeps
         * the adaptive spin. */
        int r = efs_rdma_reply_ready_quick(rc);
        if (r < 0)
            return wait_teardown(c, "rdma conn broken (quick check)");
        if (r > 0)
            return EFS_CONN_RDMA;
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        if (poll(&p, 1, 0) < 0)
            return wait_teardown(c, "poll(0) failed");
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return wait_teardown(c, "tcp POLLERR/HUP (non-blocking check)");
        if (p.revents & POLLIN) {
            int t = tcp_has_request(c->fd);
            if (t < 0)
                return wait_teardown(c, "tcp_has_request failed (quick)");
            if (t > 0)
                return EFS_CONN_TCP;
        }
        /* Neither channel ready: block on the TCP fd + the CQ eventfd. */
        struct pollfd pf[2] = {
            { .fd = c->fd, .events = POLLIN },
            { .fd = efs_rdma_reply_fd(rc), .events = POLLIN },
        };
        int br = poll(pf, 2, -1);
        if (br < 0) {
            if (errno == EINTR)
                continue;
            return wait_teardown(c, "poll(-1) failed");
        }
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            return wait_teardown(c, "tcp POLLERR/HUP (blocking poll)");
        /* RDMA first when both are ready: preferring TCP here blocked
         * recv_all forever. */
        if (pf[1].revents & POLLIN) {
            /* efd is level-triggered leftovers after a consumed RDMA
             * frame or a send-side wake. Only take RDMA when a recv is
             * actually queued — otherwise recv_wait(-1) blocks forever
             * and treats a later TCP request as peer death. */
            int r2 = efs_rdma_reply_ready_quick(rc);
            if (r2 < 0)
                return wait_teardown(c, "rdma conn broken (after efd wake)");
            if (r2 > 0)
                return EFS_CONN_RDMA;
            uint64_t tmp;
            if (read(efs_rdma_reply_fd(rc), &tmp, sizeof(tmp)) < 0 &&
                errno != EAGAIN)
                return wait_teardown(c, "efd read failed");
        }
        if (pf[0].revents & POLLIN) {
            int t = tcp_has_request(c->fd);
            if (t < 0)
                return wait_teardown(c, "tcp_has_request failed (blocking)");
            if (t > 0)
                return EFS_CONN_TCP;
        }
    }
}

int efs_conn_send_msg_parts(struct efs_conn *c, uint8_t type,
                            const void *part1, uint32_t part1_len,
                            const void *part2, uint32_t part2_len)
{
    uint32_t n1 = (part1_len > 0 && part1) ? part1_len : 0;
    uint32_t n2 = (part2_len > 0 && part2) ? part2_len : 0;
    uint32_t frame_len;
    if (n1 > UINT32_MAX - n2)
        return EFS_ERR_PROTO;
    if (efs_wire_frame_size(n1 + n2, &frame_len) != EFS_OK)
        return EFS_ERR_PROTO;
    if (conn_pick_send_chan(c, frame_len) == EFS_CONN_RDMA) {
        /* Recycled fd: this object still has dest_qpn for a QP the peer
         * already destroyed when the old TCP got FIN. */
        if (!efs_conn_fd_matches(c))
            return EFS_ERR_NET;
        /* No silent TCP fallback on send error: the QP is broken; the pool
         * drops and reconnects the conn. */
        if (efs_rdma_send_frame(c->rc, type, part1, n1,
                                part2, n2) != 0)
            return EFS_ERR_NET;
        c->recv_chan = EFS_CONN_RDMA;
        return EFS_OK;
    }
    c->recv_chan = EFS_CONN_TCP;
    return efs_send_msg_parts(c->fd, type, part1, part1_len,
                              part2, part2_len);
}

int efs_conn_send_msg(struct efs_conn *c, uint8_t type, const void *payload,
                      uint32_t payload_len)
{
    return efs_conn_send_msg_parts(c, type, payload, payload_len, NULL, 0);
}

/* Wait for one RDMA frame, validate the header, and return the payload
 * pointer into the recv pool buffer (valid until efs_rdma_recv_repost). */
static int conn_rdma_frame(struct efs_conn *c, uint8_t *type,
                           const uint8_t **payload, uint32_t *payload_len)
{
    int wr = efs_rdma_recv_wait(c->rc, EFS_IO_TIMEOUT_MS);
    /* A real byte on the TCP side-channel (reply larger than the RDMA
     * pool, or a request the server answered on TCP). Caller reads it
     * with the TCP recv. Anything else is a dead conn. */
    if (wr == EFS_ERR_AGAIN)
        return EFS_ERR_AGAIN;
    if (wr != 0)
        return EFS_ERR_NET;
    uint32_t flen = 0;
    uint8_t *frame = efs_rdma_recv_frame(c->rc, &flen);
    if (!frame || flen < 5) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    int rc = efs_wire_frame_decode(frame, flen, type, payload, payload_len);
    if (rc != EFS_OK) {
        efs_rdma_recv_repost(c->rc);
        return rc;
    }
    /* An RDMA pool frame can never be this big; keep the cap as a guard. */
    if (*payload_len + 1u > 16u * 1024 * 1024) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    return EFS_OK;
}

int efs_conn_recv_msg(struct efs_conn *c, uint8_t *type, void **payload,
                      uint32_t *payload_len)
{
    if (!c->rc || c->recv_chan == EFS_CONN_TCP)
        return efs_recv_msg(c->fd, type, payload, payload_len);

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);
    if (rc == EFS_ERR_AGAIN)
        return efs_recv_msg(c->fd, type, payload, payload_len);
    if (rc != EFS_OK)
        return rc;
    if (payload_len)
        *payload_len = plen;
    if (plen == 0 || !payload) {
        if (payload)
            *payload = NULL;
        efs_rdma_recv_repost(c->rc);
        return EFS_OK;
    }
    uint8_t *buf = malloc(plen);
    if (!buf) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_NOMEM;
    }
    memcpy(buf, pl, plen);
    *payload = buf;
    efs_rdma_recv_repost(c->rc);
    return EFS_OK;
}

int efs_conn_recv_msg_into(struct efs_conn *c, uint8_t *type, uint8_t *status,
                           void *hdr, uint32_t hdr_len,
                           void *body, uint32_t body_len)
{
    if (!c->rc || c->recv_chan == EFS_CONN_TCP)
        return efs_recv_msg_into(c->fd, type, status, hdr, hdr_len,
                                 body, body_len);

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);
    if (rc == EFS_ERR_AGAIN)
        return efs_recv_msg_into(c->fd, type, status, hdr, hdr_len,
                                 body, body_len);
    if (rc != EFS_OK)
        return rc;
    if (plen != 1 && plen != 1 + hdr_len + body_len) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    *status = pl[0];
    if (plen > 1) {
        if (hdr_len > 0)
            memcpy(hdr, pl + 1, hdr_len);
        if (body_len > 0)
            memcpy(body, pl + 1 + hdr_len, body_len);
    }
    efs_rdma_recv_repost(c->rc);
    return EFS_OK;
}

int efs_conn_recv_u8_reply(struct efs_conn *c, uint8_t *type, uint8_t *status)
{
    if (!c->rc || c->recv_chan == EFS_CONN_TCP)
        return efs_recv_u8_reply(c->fd, type, status);

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);
    if (rc == EFS_ERR_AGAIN)
        return efs_recv_u8_reply(c->fd, type, status);
    if (rc != EFS_OK)
        return rc;
    /* plen 1 is status. plen 2 is a PUT reply (status, path); a status-only
     * reader keeps the first byte. */
    if (plen < 1 || plen > 4) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    if (status)
        *status = pl[0];
    efs_rdma_recv_repost(c->rc);
    return EFS_OK;
}

int efs_conn_recv_put_reply(struct efs_conn *c, uint8_t *type, uint8_t *status,
                            uint8_t *path)
{
    if (path)
        *path = 0xff;
    if (!c->rc || c->recv_chan == EFS_CONN_TCP)
        return efs_recv_put_reply(c->fd, type, status, path);

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);
    if (rc == EFS_ERR_AGAIN)
        return efs_recv_put_reply(c->fd, type, status, path);
    if (rc != EFS_OK)
        return rc;
    if (plen < 1 || plen > 4) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    if (status)
        *status = pl[0];
    if (path && plen >= 2)
        *path = pl[1];
    efs_rdma_recv_repost(c->rc);
    return EFS_OK;
}

int efs_conn_reply_watch(struct efs_conn *c)
{
    if (!c->rc || c->recv_chan == EFS_CONN_TCP) {
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        if (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLERR | POLLHUP)))
            return EFS_CONN_REPLY_READY;
        return c->fd;
    }
    int r = efs_rdma_reply_ready(c->rc);
    if (r > 0)
        return EFS_CONN_REPLY_READY;
    if (r < 0)
        return -1;
    return efs_rdma_reply_fd(c->rc);
}

int efs_conn_reply_watch_quick(struct efs_conn *c)
{
    if (!c->rc || c->recv_chan == EFS_CONN_TCP)
        return efs_conn_reply_watch(c); /* TCP poll(0) is already spin-free */
    int r = efs_rdma_reply_ready_quick(c->rc);
    if (r > 0)
        return EFS_CONN_REPLY_READY;
    if (r < 0)
        return -1;
    return efs_rdma_reply_fd(c->rc);
}

int efs_conn_reply_watch_us(struct efs_conn *c, int budget_us)
{
    if (!c->rc || c->recv_chan == EFS_CONN_TCP)
        return efs_conn_reply_watch(c); /* TCP poll(0) is already spin-free */
    int r = efs_rdma_reply_ready_us(c->rc, budget_us);
    if (r > 0)
        return EFS_CONN_REPLY_READY;
    if (r < 0)
        return -1;
    return efs_rdma_reply_fd(c->rc);
}
