#ifndef EFS_TRANSPORT_H
#define EFS_TRANSPORT_H

#include "efs/common.h"
#include <stdint.h>

/* Framed-message transport. Production TCP/RDMA lives in protocol.c
 * (efs_conn_send_msg / efs_conn_recv_msg); this vtable is the seam the
 * simulator implements with a message queue, and that later meta/data
 * callers use without knowing sockets vs RDMA vs loopback.
 *
 * Frame layout is efs/wire.h. recv mallocs *payload (caller free());
 * empty payload is NULL with plen 0. */

struct efs_conn;

struct efs_transport_ops {
    int (*send)(void *ctx, uint8_t type,
                const void *p1, uint32_t n1,
                const void *p2, uint32_t n2);
    int (*recv)(void *ctx, uint8_t *type, void **payload, uint32_t *plen);
    /* EFS_OK when a request is readable; EFS_ERR_AGAIN if the loop
     * backend's queue is empty; EFS_ERR_NET on peer close. */
    int (*wait_request)(void *ctx);
    void (*destroy)(void *ctx);
};

struct efs_transport {
    const struct efs_transport_ops *ops;
    void *ctx;
};

static inline int efs_transport_send_parts(struct efs_transport *t, uint8_t type,
                                           const void *p1, uint32_t n1,
                                           const void *p2, uint32_t n2)
{
    if (!t || !t->ops || !t->ops->send)
        return EFS_ERR_INVAL;
    return t->ops->send(t->ctx, type, p1, n1, p2, n2);
}

static inline int efs_transport_send(struct efs_transport *t, uint8_t type,
                                     const void *payload, uint32_t len)
{
    return efs_transport_send_parts(t, type, payload, len, NULL, 0);
}

static inline int efs_transport_recv(struct efs_transport *t, uint8_t *type,
                                     void **payload, uint32_t *plen)
{
    if (!t || !t->ops || !t->ops->recv)
        return EFS_ERR_INVAL;
    return t->ops->recv(t->ctx, type, payload, plen);
}

static inline int efs_transport_wait_request(struct efs_transport *t)
{
    if (!t || !t->ops || !t->ops->wait_request)
        return EFS_ERR_INVAL;
    return t->ops->wait_request(t->ctx);
}

static inline void efs_transport_destroy(struct efs_transport *t)
{
    if (!t)
        return;
    if (t->ops && t->ops->destroy)
        t->ops->destroy(t->ctx);
    t->ops = NULL;
    t->ctx = NULL;
}

/* In-process paired queues (simulator / unit tests). Single-threaded:
 * send on A, recv on B. wait_request is EFS_OK if queued, else AGAIN. */
int efs_transport_loop_pair(struct efs_transport **a, struct efs_transport **b);
void efs_transport_loop_free(struct efs_transport *t);

/* Adapter over an existing efs_conn (TCP or RDMA). If own is nonzero,
 * destroy closes the conn. Exported send/recv symbols stay in protocol.c. */
struct efs_transport *efs_transport_from_conn(struct efs_conn *c, int own);
void efs_transport_conn_free(struct efs_transport *t);

#endif
