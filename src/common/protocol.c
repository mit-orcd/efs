#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
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
    uint32_t payload_len = part1_len + part2_len;
    uint32_t len = htonl(1 + payload_len);
    struct iovec iov[4];
    int niov = 2;
    iov[0].iov_base = &len;
    iov[0].iov_len = sizeof(len);
    iov[1].iov_base = &type;
    iov[1].iov_len = 1;
    if (part1_len > 0 && part1) {
        iov[niov].iov_base = (void *)part1;
        iov[niov].iov_len = part1_len;
        niov++;
    }
    if (part2_len > 0 && part2) {
        iov[niov].iov_base = (void *)part2;
        iov[niov].iov_len = part2_len;
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
    uint32_t len;
    if (efs_recv_all(fd, &len, sizeof(len)) != 0)
        return EFS_ERR_NET;
    len = ntohl(len);
    if (len != 2)
        return EFS_ERR_PROTO;
    uint8_t buf[2];
    if (efs_recv_all(fd, buf, 2) != 0)
        return EFS_ERR_NET;
    if (type)
        *type = buf[0];
    if (status)
        *status = buf[1];
    return EFS_OK;
}

int efs_recv_msg(int fd, uint8_t *type, void **payload, uint32_t *payload_len)
{
    uint32_t len;
    if (efs_recv_all(fd, &len, sizeof(len)) != 0)
        return EFS_ERR_NET;
    len = ntohl(len);
    if (len == 0 || len > EFS_MSG_MAX_LEN)
        return EFS_ERR_PROTO;

    uint8_t t;
    if (efs_recv_all(fd, &t, 1) != 0)
        return EFS_ERR_NET;
    *type = t;

    uint32_t plen = len - 1;
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
    uint32_t len;
    if (efs_recv_all(fd, &len, sizeof(len)) != 0)
        return EFS_ERR_NET;
    len = ntohl(len);
    if (len == 0 || len > EFS_MSG_MAX_LEN)
        return EFS_ERR_PROTO;

    uint8_t t;
    if (efs_recv_all(fd, &t, 1) != 0)
        return EFS_ERR_NET;
    *type = t;

    uint32_t plen = len - 1;
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
 *  - client: RDMA when the frame fits, except GET_META whose reply is
 *    unbounded — that exchange stays on TCP so recv_chan is deterministic. */
static int conn_pick_send_chan(struct efs_conn *c, uint8_t type,
                               uint32_t frame_len)
{
    if (!c->rc)
        return EFS_CONN_TCP;
    if (frame_len > efs_rdma_max_frame(c->rc))
        return EFS_CONN_TCP;
    if (c->is_server)
        return c->recv_chan == EFS_CONN_RDMA ? EFS_CONN_RDMA : EFS_CONN_TCP;
    if (type == EFS_MSG_GET_META)
        return EFS_CONN_TCP;
    return EFS_CONN_RDMA;
}

int efs_conn_send_msg_parts(struct efs_conn *c, uint8_t type,
                            const void *part1, uint32_t part1_len,
                            const void *part2, uint32_t part2_len)
{
    uint32_t frame_len = 5 + part1_len + part2_len;
    if (conn_pick_send_chan(c, type, frame_len) == EFS_CONN_RDMA) {
        /* No silent TCP fallback on send error: the QP is broken; the pool
         * drops and reconnects the conn. */
        if (efs_rdma_send_frame(c->rc, type, part1, part1_len,
                                part2, part2_len) != 0)
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
    if (efs_rdma_recv_wait(c->rc, EFS_IO_TIMEOUT_MS) != 0)
        return EFS_ERR_NET;
    uint32_t flen = 0;
    uint8_t *frame = efs_rdma_recv_frame(c->rc, &flen);
    if (!frame || flen < 5) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    uint32_t nlen;
    memcpy(&nlen, frame, 4);
    nlen = ntohl(nlen);
    if (nlen == 0 || nlen > 16 * 1024 * 1024 || flen != 4 + nlen) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    *type = frame[4];
    *payload = frame + 5;
    *payload_len = nlen - 1;
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
    if (rc != EFS_OK)
        return rc;
    if (plen != 1) {
        efs_rdma_recv_repost(c->rc);
        return EFS_ERR_PROTO;
    }
    if (status)
        *status = pl[0];
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
