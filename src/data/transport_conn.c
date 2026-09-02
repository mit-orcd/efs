#include "efs/transport.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include <stdlib.h>

struct conn_end {
    struct efs_conn *c;
    int own;
};

static int conn_send(void *ctx, uint8_t type,
                     const void *p1, uint32_t n1,
                     const void *p2, uint32_t n2)
{
    struct conn_end *e = ctx;
    if (!e || !e->c)
        return EFS_ERR_INVAL;
    return efs_conn_send_msg_parts(e->c, type, p1, n1, p2, n2);
}

static int conn_recv(void *ctx, uint8_t *type, void **payload, uint32_t *plen)
{
    struct conn_end *e = ctx;
    if (!e || !e->c)
        return EFS_ERR_INVAL;
    return efs_conn_recv_msg(e->c, type, payload, plen);
}

static int conn_wait(void *ctx)
{
    struct conn_end *e = ctx;
    int ch;
    if (!e || !e->c)
        return EFS_ERR_INVAL;
    ch = efs_conn_wait_request(e->c);
    if (ch < 0)
        return EFS_ERR_NET;
    return EFS_OK;
}

static void conn_destroy(void *ctx)
{
    struct conn_end *e = ctx;
    if (!e)
        return;
    if (e->own && e->c)
        efs_conn_destroy(e->c);
    free(e);
}

static const struct efs_transport_ops conn_ops = {
    .send = conn_send,
    .recv = conn_recv,
    .wait_request = conn_wait,
    .destroy = conn_destroy,
};

struct efs_transport *efs_transport_from_conn(struct efs_conn *c, int own)
{
    struct efs_transport *t;
    struct conn_end *e;

    if (!c)
        return NULL;
    t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    e = calloc(1, sizeof(*e));
    if (!e) {
        free(t);
        return NULL;
    }
    e->c = c;
    e->own = own ? 1 : 0;
    t->ops = &conn_ops;
    t->ctx = e;
    return t;
}

void efs_transport_conn_free(struct efs_transport *t)
{
    if (!t)
        return;
    efs_transport_destroy(t);
    free(t);
}
