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
#include <limits.h>
#include <stddef.h>
#include <fcntl.h>

static int send_iov_run(int fd, struct iovec *iov, int niov)
{
    int i = 0;
    while (i < niov) {
        int bounded = efs_net_deadline_ms() != 0;
        if (bounded && !efs_net_remaining_ms(INT_MAX)) {
            errno = ETIMEDOUT;
            return EFS_ERR_NET;
        }
        ssize_t n;
        if (bounded) {
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = &iov[i];
            msg.msg_iovlen = niov - i;
            n = sendmsg(fd, &msg, MSG_DONTWAIT);
        } else {
            n = writev(fd, &iov[i], niov - i);
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (bounded && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int remaining = efs_net_remaining_ms(INT_MAX);
                if (!remaining) {
                    errno = ETIMEDOUT;
                    return EFS_ERR_NET;
                }
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                int ready = poll(&pfd, 1, remaining);
                if (ready > 0 || (ready < 0 && errno == EINTR))
                    continue;
                if (!ready) errno = ETIMEDOUT;
            }
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

static int send_iov(int fd, struct iovec *iov, int niov)
{
#ifdef __APPLE__
    /* Darwin may wait inside sendmsg despite MSG_DONTWAIT. The checked-out
     * connection is exclusive; restore its flags before returning ownership. */
    int flags = -1;
    if (efs_net_deadline_ms()) {
        flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return EFS_ERR_NET;
    }
    int rc = send_iov_run(fd, iov, niov);
    int saved = errno;
    if (flags >= 0 && fcntl(fd, F_SETFL, flags) < 0)
        return EFS_ERR_NET;
    errno = saved;
    return rc;
#else
    return send_iov_run(fd, iov, niov);
#endif
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
    if (efs_wire_frame_check_nlen(nlen) != EFS_OK || (nlen < 2 || (nlen > 4 && nlen != 1 + EFS_PUT_REPLY_LEN)))
        return EFS_ERR_PROTO;
    uint8_t raw[1 + EFS_PUT_REPLY_LEN];
    if (efs_recv_all(fd, raw, nlen) != 0)
        return EFS_ERR_NET;
    if (type)
        *type = raw[0];
    if (status)
        *status = raw[1];
    return EFS_OK;
}

int efs_recv_put_reply(int fd, uint8_t *type, uint8_t *status, uint8_t *path, uint64_t *epoch)
{
    uint32_t be;
    uint32_t nlen;
    if (path)
        *path = 0xff;
    if (efs_recv_all(fd, &be, sizeof(be)) != 0)
        return EFS_ERR_NET;
    nlen = ntohl(be);
    if (efs_wire_frame_check_nlen(nlen) != EFS_OK || nlen != 1 + EFS_PUT_REPLY_LEN)
        return EFS_ERR_PROTO;
    uint8_t raw[1 + EFS_PUT_REPLY_LEN];
    if (efs_recv_all(fd, raw, nlen) != 0)
        return EFS_ERR_NET;
    if (type)
        *type = raw[0];
    if (status)
        *status = raw[1];
    if (path)
        *path = raw[2];
    if (epoch) *epoch = efs_put_epoch_read(raw + 3);
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

    /* Metadata and Raft control frames are small. Read type and payload
     * together instead of a separate one-byte socket receive. Validate the
     * length first, and keep bulk payloads landing directly at malloc base. */
    if (nlen <= 2048) {
        uint8_t raw[2048];
        if (efs_recv_all(fd, raw, nlen) != 0)
            return EFS_ERR_NET;
        *type = raw[0];
        plen = nlen - 1;
        if (payload_len)
            *payload_len = plen;
        if (payload) {
            *payload = NULL;
            if (plen) {
                uint8_t *buf = malloc(plen);
                if (!buf)
                    return EFS_ERR_NOMEM;
                memcpy(buf, raw + 1, plen);
                *payload = buf;
            }
        }
        return EFS_OK;
    }

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
    if (hdr_len>UINT32_MAX-1 || body_len>UINT32_MAX-1-hdr_len ||
        (plen != 1 && plen != 1 + hdr_len + body_len)) {
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

/* Upgraded connections have one application channel regardless of size.
 * auto may fall back only before successful setup. */
static int conn_pick_send_chan(struct efs_conn *c,uint32_t frame_len)
{ (void)frame_len;return c->rc?EFS_CONN_RDMA:EFS_CONN_TCP; }

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
        int r = efs_rdma_message_ready(rc);
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
            if (t > 0) {
                efs_rdma_tcp_violation(c->rc);
                return wait_teardown(c,"unexpected TCP application byte after upgrade");
            }
        }
        /* Neither channel ready: block on the TCP fd + the CQ eventfd. */
        struct pollfd pf[2] = {
            { .fd = c->fd, .events = POLLIN },
            { .fd = efs_rdma_reply_fd(rc), .events = POLLIN },
        };
        int br = poll(pf, 2, efs_rdma_message_poll_ms(rc));
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
            int r2 = efs_rdma_message_ready(rc);
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
            if (t > 0) {
                efs_rdma_tcp_violation(c->rc);
                return wait_teardown(c,"unexpected TCP application byte after upgrade");
            }
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
        efs_conn_note_ok(c);
        return EFS_OK;
    }
    c->recv_chan = EFS_CONN_TCP;
    if (efs_send_msg_parts(c->fd, type, part1, part1_len,
                           part2, part2_len) != 0)
        return EFS_ERR_NET;
    efs_conn_note_ok(c);
    return EFS_OK;
}

int efs_conn_send_msg(struct efs_conn *c, uint8_t type, const void *payload,
                      uint32_t payload_len)
{
    return efs_conn_send_msg_parts(c, type, payload, payload_len, NULL, 0);
}

/* Wait for one RDMA frame, validate the header, and return the payload
 * pointer into the recv pool buffer (valid until efs_rdma_message_release). */
static int conn_rdma_frame(struct efs_conn *c, uint8_t *type,
                           const uint8_t **payload, uint32_t *payload_len)
{
    int timeout = efs_net_remaining_ms(c->recv_timeout_ms > 0
                                      ? c->recv_timeout_ms : EFS_IO_TIMEOUT_MS);
    if (!timeout)
        return EFS_ERR_BUSY;
    int wr = efs_rdma_message_wait(c->rc, timeout);
    if (!efs_net_remaining_ms(INT_MAX)) {
        /* A successful wait owns a pool frame even if the budget expired
         * concurrently. Return that ownership before dropping the reply. */
        if (wr == EFS_OK)
            efs_rdma_message_release(c->rc);
        return EFS_ERR_BUSY;
    }
    if (wr != 0)
        return EFS_ERR_NET;
    uint32_t flen = 0;
    uint8_t *frame = efs_rdma_message_frame(c->rc, &flen);
    if (!frame || flen < 5) {
        efs_rdma_message_release(c->rc);
        return EFS_ERR_PROTO;
    }
    int rc = efs_wire_frame_decode(frame, flen, type, payload, payload_len);
    if (rc != EFS_OK) {
        efs_rdma_message_release(c->rc);
        return rc;
    }
    return EFS_OK;
}

int efs_conn_recv_msg(struct efs_conn *c, uint8_t *type, void **payload,
                      uint32_t *payload_len)
{
    if (!c->rc) {
        int rc = efs_recv_msg(c->fd, type, payload, payload_len);
        c->last_recv_chan = EFS_CONN_TCP;
        if (rc == 0)
            efs_conn_note_ok(c);
        return rc;
    }

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);

    if (rc != EFS_OK)
        return rc;
    c->last_recv_chan = EFS_CONN_RDMA;
    if (payload_len)
        *payload_len = plen;
    if (plen == 0 || !payload) {
        if (payload)
            *payload = NULL;
        efs_rdma_message_release(c->rc);
        efs_conn_note_ok(c);
        return EFS_OK;
    }
    *payload = efs_rdma_message_take(c->rc);
    efs_conn_note_ok(c);
    return EFS_OK;
}

int efs_conn_recv_msg_into(struct efs_conn *c, uint8_t *type, uint8_t *status,
                           void *hdr, uint32_t hdr_len,
                           void *body, uint32_t body_len)
{
    if (!c->rc) {
        c->last_recv_chan = EFS_CONN_TCP;
        return efs_recv_msg_into(c->fd, type, status, hdr, hdr_len,
                                 body, body_len);
    }

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);

    if (rc != EFS_OK)
        return rc;
    c->last_recv_chan = EFS_CONN_RDMA;
    if (hdr_len>UINT32_MAX-1 || body_len>UINT32_MAX-1-hdr_len ||
        (plen != 1 && plen != 1 + hdr_len + body_len)) {
        efs_rdma_message_release(c->rc);
        return EFS_ERR_PROTO;
    }
    *status = pl[0];
    if (plen > 1) {
        if (hdr_len > 0)
            memcpy(hdr, pl + 1, hdr_len);
        if (body_len > 0)
            memcpy(body, pl + 1 + hdr_len, body_len);
    }
    efs_rdma_message_release(c->rc);
    return EFS_OK;
}

int efs_conn_recv_u8_reply(struct efs_conn *c, uint8_t *type, uint8_t *status)
{
    if (!c->rc)
        return efs_recv_u8_reply(c->fd, type, status);

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);

    if (rc != EFS_OK)
        return rc;
    /* plen 1 is status. plen 2 is a PUT reply (status, path); a status-only
     * reader keeps the first byte. */
    if (plen < 1 || (plen > 4 && plen != EFS_PUT_REPLY_LEN)) {
        efs_rdma_message_release(c->rc);
        return EFS_ERR_PROTO;
    }
    if (status)
        *status = pl[0];
    efs_rdma_message_release(c->rc);
    return EFS_OK;
}

int efs_conn_recv_put_reply(struct efs_conn *c, uint8_t *type, uint8_t *status,
                            uint8_t *path, uint64_t *epoch)
{
    if (path)
        *path = 0xff;
    if (!c->rc) {
        if (c->put_reply_used) {
            if (c->put_reply_used < 4)
                return EFS_ERR_PROTO;
            uint32_t be;
            memcpy(&be, c->put_reply, sizeof(be));
            uint32_t nlen = ntohl(be);
            if (nlen != 1 + EFS_PUT_REPLY_LEN || c->put_reply_used != 4 + nlen)
                return EFS_ERR_PROTO;
            if (type)
                *type = c->put_reply[4];
            if (status)
                *status = c->put_reply[5];
            if (path)
                *path = c->put_reply[6];
            if (epoch) *epoch = efs_put_epoch_read(c->put_reply + 7);
            c->put_reply_used = 0;
            efs_conn_note_ok(c);
            return EFS_OK;
        }
        return efs_recv_put_reply(c->fd, type, status, path, epoch);
    }

    const uint8_t *pl = NULL;
    uint32_t plen = 0;
    int rc = conn_rdma_frame(c, type, &pl, &plen);

    if (rc != EFS_OK)
        return rc;
    if (plen != EFS_PUT_REPLY_LEN) {
        efs_rdma_message_release(c->rc);
        return EFS_ERR_PROTO;
    }
    if (status)
        *status = pl[0];
    if (path)
        *path = pl[1];
    if (epoch) *epoch = efs_put_epoch_read(pl + 2);
    efs_rdma_message_release(c->rc);
    return EFS_OK;
}

int efs_conn_reply_watch(struct efs_conn *c)
{
    if (!c->rc) {
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        if (poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLERR | POLLHUP)))
            return EFS_CONN_REPLY_READY;
        return c->fd;
    }
    int r = efs_rdma_message_ready(c->rc);
    if (r > 0)
        return EFS_CONN_REPLY_READY;
    if (r < 0)
        return -1;
    return efs_rdma_reply_fd(c->rc);
}

int efs_conn_reply_watch_quick(struct efs_conn *c)
{
    if (!c->rc)
        return efs_conn_reply_watch(c); /* TCP poll(0) is already spin-free */
    int r = efs_rdma_message_ready(c->rc);
    if (r > 0)
        return EFS_CONN_REPLY_READY;
    if (r < 0)
        return -1;
    return efs_rdma_reply_fd(c->rc);
}

int efs_conn_reply_watch_us(struct efs_conn *c, int budget_us)
{
    (void)budget_us;
    if (!c->rc)
        return efs_conn_reply_watch(c); /* TCP poll(0) is already spin-free */
    int r = efs_rdma_message_ready(c->rc);
    if (r > 0)
        return EFS_CONN_REPLY_READY;
    if (r < 0)
        return -1;
    return efs_rdma_reply_fd(c->rc);
}

/* Only the checked-out PUT owns this buffer. Consume exactly one small frame,
 * leaving any subsequent frame in the socket. Partial TCP delivery must not
 * turn a readiness notification into a blocking recv across the whole quorum. */
int efs_conn_put_reply_watch(struct efs_conn *c, int budget_us)
{
    if (c->rc)
        return budget_us > 0 ? efs_conn_reply_watch_us(c, budget_us) :
                               efs_conn_reply_watch_quick(c);
    for (;;) {
        uint32_t target = 4;
        if (c->put_reply_used >= 4) {
            uint32_t be;
            memcpy(&be, c->put_reply, sizeof(be));
            uint32_t nlen = ntohl(be);
            if (nlen != 1 + EFS_PUT_REPLY_LEN)
                return -1;
            target += nlen;
        }
        if (c->put_reply_used == target && target > 4)
            return EFS_CONN_REPLY_READY;
        if (c->put_reply_used > target)
            return -1;
        ssize_t got = recv(c->fd, c->put_reply + c->put_reply_used,
                            target - c->put_reply_used, MSG_DONTWAIT);
        if (got > 0) {
            c->put_reply_used += (uint8_t)got;
            continue;
        }
        if (!got)
            return -1;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return c->fd;
        return -1;
    }
}

static uint8_t g_declare_role = EFS_ROLE_CLIENT;
static struct efs_identity g_declare_id;

void efs_declare_set_identity(uint8_t role, const struct efs_identity *id)
{
    g_declare_role = role;
    if (id)
        g_declare_id = *id;
    else
        memset(&g_declare_id, 0, sizeof(g_declare_id));
}

static void declare_hello(struct efs_msg_client_hello *h, uint8_t role,
                          const struct efs_identity *id)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "EFSP", 4);
    h->version = EFS_PROTOCOL_VERSION;
    h->role = role;
    if (id)
        h->id = *id;
}

static int declare_check(uint8_t type, const void *reply, uint32_t len,
                         struct efs_msg_client_hello_reply *out)
{
    const struct efs_msg_client_hello_reply *r = reply;
    if (type != EFS_MSG_CLIENT_HELLO_REPLY || len != sizeof(*r)) return EFS_ERR_PROTO;
    if (out) *out = *r;
    if (r->rc != EFS_OK) return r->rc;
    /* One supported protocol: a server on any other version is refused,
     * even one that accepted us (a fail-open server must not be used). */
    return r->version == EFS_PROTOCOL_VERSION ? EFS_OK : EFS_ERR_PROTO;
}
int efs_conn_declare_as(struct efs_conn *c, uint8_t role, const struct efs_identity *id,
                        struct efs_msg_client_hello_reply *out)
{
    struct efs_msg_client_hello h;
    uint8_t type = 0;
    void *reply = NULL;
    uint32_t len = 0;
    if (!c) return EFS_ERR_INVAL;
    declare_hello(&h, role, id);
    if (efs_conn_send_msg(c, EFS_MSG_CLIENT_HELLO, &h, sizeof(h)) ||
        efs_conn_recv_msg(c, &type, &reply, &len)) {
        free(reply);
        return EFS_ERR_NET;
    }
    int rc = declare_check(type, reply, len, out);
    free(reply);
    return rc;
}
int efs_conn_declare(struct efs_conn *c)
{
    return efs_conn_declare_as(c, g_declare_role, &g_declare_id, NULL);
}
int efs_fd_declare(int fd)
{
    struct efs_msg_client_hello h;
    uint8_t type = 0;
    void *reply = NULL;
    uint32_t len = 0;
    declare_hello(&h, g_declare_role, &g_declare_id);
    if (efs_send_msg(fd, EFS_MSG_CLIENT_HELLO, &h, sizeof(h)) ||
        efs_recv_msg(fd, &type, &reply, &len)) {
        free(reply);
        return EFS_ERR_NET;
    }
    int rc = declare_check(type, reply, len, NULL);
    free(reply);
    return rc;
}

int efs_connect_declared(const char *host, uint16_t port)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;
    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
    int rc = efs_fd_declare(fd);
    if (rc != EFS_OK) {
        fprintf(stderr, "efs: %s:%u refused or did not answer the protocol %u "
                        "declaration (rc=%d)\n", host, port, EFS_PROTOCOL_VERSION, rc);
        close(fd);
        return -1;
    }
    efs_set_recv_timeout(fd, 0);
    efs_set_send_timeout(fd, 0);
    return fd;
}

/* W133 server-side gate. */
int efs_declaration_required(uint8_t type)
{
    return type != EFS_MSG_CLIENT_HELLO && type != EFS_MSG_HEARTBEAT;
}

int efs_client_hello_check(const void *payload, uint32_t len)
{
    const struct efs_msg_client_hello *h = payload;
    if (!payload || len != sizeof(*h) || memcmp(h->magic, "EFSP", 4) != 0)
        return EFS_ERR_PROTO;
    if (h->version != EFS_PROTOCOL_VERSION)
        return EFS_ERR_PROTO;
    return h->role == EFS_ROLE_CLIENT || h->role == EFS_ROLE_MGMT || h->role == EFS_ROLE_PEER
               ? EFS_OK : EFS_ERR_PROTO;
}

static const struct efs_admission_ops *g_admission;

void efs_decl_gate_set_admission(const struct efs_admission_ops *ops)
{
    __atomic_store_n(&g_admission, ops, __ATOMIC_RELEASE);
}

/* D39 C4 allowlists. Status-class reads are control for every role;
 * RAFT_MKFS with an empty payload is the formation command, with a payload
 * it submits a metadata command (application work). */
static int status_class(uint8_t type)
{
    switch (type) {
    case EFS_MSG_STATUS: case EFS_MSG_RAFT_STATUS: case EFS_MSG_VERSION:
    case EFS_MSG_LIST_NODES: case EFS_MSG_GET_FEATURES: case EFS_MSG_IO_STATS:
    case EFS_MSG_SYNC_STATS: case EFS_MSG_GC_STATUS: case EFS_MSG_QUERY_STATS:
    case EFS_MSG_HEARTBEAT: case EFS_MSG_RDMA_SETUP:
        return 1;
    default:
        return 0;
    }
}
static int client_app(uint8_t type)
{
    switch (type) {
    case EFS_MSG_GET_CHUNK: case EFS_MSG_PUT_CHUNK: case EFS_MSG_GET_SEGMENT:
    case EFS_MSG_INODE_LOOKUP: case EFS_MSG_INODE_CREATE: case EFS_MSG_INODE_GETATTR:
    case EFS_MSG_INODE_READDIR: case EFS_MSG_INODE_UNLINK: case EFS_MSG_INODE_SETATTR:
    case EFS_MSG_INODE_LINK: case EFS_MSG_REPORT_CHUNKS: case EFS_MSG_INODE_GETCHUNKS:
    case EFS_MSG_INODE_APPEND: case EFS_MSG_INODE_LOOKUP_PATH: case EFS_MSG_INODE_HOLD:
    case EFS_MSG_INODE_FLOCK: case EFS_MSG_INODE_RENAME_AT: case EFS_MSG_XATTR:
    case EFS_MSG_INODE_WRITER_VIEW: case EFS_MSG_LANE_WRITER_VIEW: case EFS_MSG_LANE_BOOTSTRAP:
    case EFS_MSG_PUBLICATION: case EFS_MSG_PUBLICATION_STATUS: case EFS_MSG_PUBLICATION_RETIRE:
    case EFS_MSG_QUERY_SNAPSHOT: case EFS_MSG_CREATE_RETAINED: case EFS_MSG_DATA_COMMIT:
    case EFS_MSG_BENCH_PUT:
        return 1;
    default:
        return 0;
    }
}
/* Daemon-to-daemon data plane: peers only, as application work. */
static int peer_app(uint8_t type)
{
    switch (type) {
    case EFS_MSG_PUT_SEGMENT: case EFS_MSG_GC_INODE_BATCH: case EFS_MSG_GC_SEGMENT_BATCH:
    case EFS_MSG_GC_FRAG_BATCH: case EFS_MSG_STORAGE_SESSION_FENCE: case EFS_MSG_PACK_OBJECT_READ:
    case EFS_MSG_REBUILD_PUT:
        return 1;
    default:
        return 0;
    }
}
/* Maintenance commands (C12 leaves their retry rules unresolved): served
 * like the client protocol, only once formation is complete. RAFT_STEP_DOWN
 * is the model's disruptive mgmt command and stays mgmt-only. */
static int mgmt_maintenance(uint8_t type)
{
    switch (type) {
    case EFS_MSG_PACK_COMPACT: case EFS_MSG_RAFT_RESYNC: case EFS_MSG_SHRINK_QUOTA:
    case EFS_MSG_ADD_STORAGE:
        return 1;
    default:
        return 0;
    }
}

int efs_role_allow(uint8_t role, uint8_t type, uint32_t len)
{
    int mkfs = type == EFS_MSG_RAFT_MKFS && len == 0;
    int submit = type == EFS_MSG_RAFT_MKFS && len > 0;
    switch (role) {
    case EFS_ROLE_PEER:
        if (status_class(type) || type == EFS_MSG_RAFT || type == EFS_MSG_HELLO ||
            type == EFS_MSG_FORMATION_READ)
            return EFS_ALLOW_CONTROL;
        if (client_app(type) || peer_app(type) || submit)
            return EFS_ALLOW_APP;
        return EFS_ALLOW_NONE;
    case EFS_ROLE_MGMT:
        if (status_class(type) || mkfs || type == EFS_MSG_FORMATION_READ ||
            type == EFS_MSG_RAFT_STEP_DOWN)
            return EFS_ALLOW_MGMT;
        if (client_app(type) || submit || mgmt_maintenance(type))
            return EFS_ALLOW_APP;
        return EFS_ALLOW_NONE;
    case EFS_ROLE_CLIENT:
        if (status_class(type) || client_app(type) || submit || mgmt_maintenance(type))
            return EFS_ALLOW_APP;
        return EFS_ALLOW_NONE;
    default:
        return EFS_ALLOW_NONE;
    }
}

/* One audible line per refused connection, bounded under a flood: the
 * first 10 and then every 1000th. */
static void gate_refuse_log(const char *what, unsigned detail)
{
    static uint64_t refused;
    uint64_t n = __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
    if (n <= 10 || !(n % 1000))
        fprintf(stderr, "handler: refused %s %u; this daemon serves only after a "
                        "protocol %u declaration; closing (%llu so far)\n",
                what, detail, EFS_PROTOCOL_VERSION, (unsigned long long)n);
}

int efs_decl_gate_frame(struct efs_decl_gate *g, struct efs_conn *c, uint8_t type,
                        const void *payload, uint32_t len)
{
    const struct efs_admission_ops *ops = __atomic_load_n(&g_admission, __ATOMIC_ACQUIRE);
    if (type == EFS_MSG_CLIENT_HELLO) {
        /* Always answered, so a mismatched or refused declarer learns the
         * served version and the responder; a refusal closes the connection.
         * One declaration per connection: a second one is refused. */
        struct efs_msg_client_hello_reply rep;
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_ERR_PROTO;
        rep.version = EFS_PROTOCOL_VERSION;
        rep.role = EFS_ROLE_PEER;
        int ok = !g->declared && efs_client_hello_check(payload, len) == EFS_OK;
        if (ok) {
            const struct efs_msg_client_hello *h = payload;
            struct efs_decl_gate next;
            memset(&next, 0, sizeof(next));
            if (ops) {
                rep.rc = ops->declare(h, &rep, &next);
            } else if (h->role == EFS_ROLE_CLIENT) {
                rep.rc = EFS_OK;
                next.role = EFS_ROLE_CLIENT;
                next.channel = EFS_CHANNEL_CLIENT;
            } else {
                rep.rc = EFS_ERR_UNSUPPORTED; /* no admission: client role only */
            }
            ok = rep.rc == EFS_OK;
            if (ok) {
                next.declared = 1;
                *g = next;
                rep.channel = g->channel;
            }
        }
        efs_conn_send_msg(c, EFS_MSG_CLIENT_HELLO_REPLY, &rep, sizeof(rep));
        if (ok)
            return EFS_GATE_ANSWERED;
        const struct efs_msg_client_hello *h = payload;
        gate_refuse_log("declaration of protocol",
                        len >= offsetof(struct efs_msg_client_hello, version) + sizeof(h->version)
                            ? h->version : 0u);
        return EFS_GATE_REFUSE;
    }
    if (!g->declared) {
        if (efs_declaration_required(type)) {
            gate_refuse_log("message", type);
            return EFS_GATE_REFUSE;
        }
        return EFS_GATE_DISPATCH;
    }
    if (type == EFS_MSG_HEARTBEAT)
        return EFS_GATE_DISPATCH;
    if (ops)
        return ops->frame(g, type, payload, len);
    if (efs_role_allow(g->role, type, len) == EFS_ALLOW_NONE) {
        gate_refuse_log("message outside the role allowlist", type);
        return EFS_GATE_REFUSE;
    }
    return EFS_GATE_DISPATCH;
}
