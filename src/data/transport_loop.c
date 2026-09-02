#include "efs/transport.h"
#include <stdlib.h>
#include <string.h>

struct loop_msg {
    uint8_t type;
    uint32_t len;
    uint8_t *payload;
    struct loop_msg *next;
};

struct loop_q {
    struct loop_msg *head;
    struct loop_msg *tail;
};

struct loop_pair {
    struct loop_q q[2];
    int refs;
};

struct loop_end {
    struct loop_pair *pair;
    int out; /* index of the queue this end sends into (peer's in) */
};

static void q_push(struct loop_q *q, struct loop_msg *m)
{
    m->next = NULL;
    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
}

static struct loop_msg *q_pop(struct loop_q *q)
{
    struct loop_msg *m = q->head;
    if (!m)
        return NULL;
    q->head = m->next;
    if (!q->head)
        q->tail = NULL;
    m->next = NULL;
    return m;
}

static void q_drain(struct loop_q *q)
{
    struct loop_msg *m;
    while ((m = q_pop(q))) {
        free(m->payload);
        free(m);
    }
}

static int loop_send(void *ctx, uint8_t type,
                     const void *p1, uint32_t n1,
                     const void *p2, uint32_t n2)
{
    struct loop_end *e = ctx;
    struct loop_msg *m;
    uint32_t n;
    uint8_t *buf = NULL;

    if (!e || !e->pair)
        return EFS_ERR_INVAL;
    if (n1 > 0 && !p1)
        return EFS_ERR_INVAL;
    if (n2 > 0 && !p2)
        return EFS_ERR_INVAL;
    if (n1 > UINT32_MAX - n2)
        return EFS_ERR_PROTO;
    n = n1 + n2;
    if (n > 0) {
        buf = malloc(n);
        if (!buf)
            return EFS_ERR_NOMEM;
        if (n1)
            memcpy(buf, p1, n1);
        if (n2)
            memcpy(buf + n1, p2, n2);
    }
    m = calloc(1, sizeof(*m));
    if (!m) {
        free(buf);
        return EFS_ERR_NOMEM;
    }
    m->type = type;
    m->len = n;
    m->payload = buf;
    q_push(&e->pair->q[e->out], m);
    return EFS_OK;
}

static int loop_recv(void *ctx, uint8_t *type, void **payload, uint32_t *plen)
{
    struct loop_end *e = ctx;
    struct loop_msg *m;
    int in;

    if (!e || !e->pair || !type)
        return EFS_ERR_INVAL;
    in = 1 - e->out;
    m = q_pop(&e->pair->q[in]);
    if (!m)
        return EFS_ERR_AGAIN;
    *type = m->type;
    if (plen)
        *plen = m->len;
    if (payload)
        *payload = m->payload;
    else
        free(m->payload);
    m->payload = NULL;
    free(m);
    return EFS_OK;
}

static int loop_wait(void *ctx)
{
    struct loop_end *e = ctx;
    int in;

    if (!e || !e->pair)
        return EFS_ERR_INVAL;
    in = 1 - e->out;
    if (e->pair->q[in].head)
        return EFS_OK;
    return EFS_ERR_AGAIN;
}

static void loop_destroy(void *ctx)
{
    struct loop_end *e = ctx;
    struct loop_pair *p;

    if (!e)
        return;
    p = e->pair;
    free(e);
    if (!p)
        return;
    p->refs--;
    if (p->refs > 0)
        return;
    q_drain(&p->q[0]);
    q_drain(&p->q[1]);
    free(p);
}

static const struct efs_transport_ops loop_ops = {
    .send = loop_send,
    .recv = loop_recv,
    .wait_request = loop_wait,
    .destroy = loop_destroy,
};

static struct efs_transport *make_end(struct loop_pair *p, int out)
{
    struct efs_transport *t = calloc(1, sizeof(*t));
    struct loop_end *e;

    if (!t)
        return NULL;
    e = calloc(1, sizeof(*e));
    if (!e) {
        free(t);
        return NULL;
    }
    e->pair = p;
    e->out = out;
    t->ops = &loop_ops;
    t->ctx = e;
    p->refs++;
    return t;
}

int efs_transport_loop_pair(struct efs_transport **a, struct efs_transport **b)
{
    struct loop_pair *p;
    struct efs_transport *ta, *tb;

    if (!a || !b)
        return EFS_ERR_INVAL;
    *a = NULL;
    *b = NULL;
    p = calloc(1, sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    ta = make_end(p, 0);
    tb = make_end(p, 1);
    if (!ta || !tb) {
        if (ta)
            efs_transport_destroy(ta);
        if (tb)
            efs_transport_destroy(tb);
        free(ta);
        free(tb);
        if (!ta && !tb)
            free(p);
        return EFS_ERR_NOMEM;
    }
    *a = ta;
    *b = tb;
    return EFS_OK;
}

void efs_transport_loop_free(struct efs_transport *t)
{
    if (!t)
        return;
    efs_transport_destroy(t);
    free(t);
}
