#include "efs/store.h"
#include <stdlib.h>
#include <string.h>

struct mem_rec {
    struct efs_frag_id id;
    uint32_t len;
    uint8_t *data;
    uint8_t sum[EFS_HASH_SIZE];
    int has_sum;
    struct mem_rec *next;
};

#define MEM_STORE_MAGIC 0x4d454d31u /* 'MEM1' */

struct mem_store {
    uint32_t magic;
    struct mem_rec *head;
    uint32_t nrec;
};

static int id_eq(const struct efs_frag_id *a, const struct efs_frag_id *b)
{
    return a->export_id == b->export_id && a->ino == b->ino &&
           a->chunk_index == b->chunk_index &&
           a->fragment_index == b->fragment_index;
}

static struct mem_rec **find_slot(struct mem_store *m, const struct efs_frag_id *id)
{
    struct mem_rec **pp = &m->head;
    while (*pp) {
        if (id_eq(&(*pp)->id, id))
            return pp;
        pp = &(*pp)->next;
    }
    return pp;
}

static int mem_get(void *ctx, const struct efs_frag_id *id,
                   uint8_t *buf, uint32_t *len,
                   uint8_t sum[EFS_HASH_SIZE], int *sum_ok)
{
    struct mem_store *m = ctx;
    struct mem_rec **pp;

    if (!m || !id || !len)
        return EFS_ERR_INVAL;
    pp = find_slot(m, id);
    if (!*pp)
        return EFS_ERR_NOT_FOUND;
    if (*len < (*pp)->len)
        return EFS_ERR_INVAL;
    if ((*pp)->len > 0 && !buf)
        return EFS_ERR_INVAL;
    if ((*pp)->len > 0)
        memcpy(buf, (*pp)->data, (*pp)->len);
    *len = (*pp)->len;
    if (sum_ok)
        *sum_ok = (*pp)->has_sum;
    if (sum && (*pp)->has_sum)
        memcpy(sum, (*pp)->sum, EFS_HASH_SIZE);
    return EFS_OK;
}

static int mem_put(void *ctx, const struct efs_frag_id *id,
                   const uint8_t *buf, uint32_t len,
                   const uint8_t sum[EFS_HASH_SIZE])
{
    struct mem_store *m = ctx;
    struct mem_rec **pp;
    struct mem_rec *r;
    uint8_t *copy = NULL;

    if (!m || !id || (len > 0 && !buf))
        return EFS_ERR_INVAL;
    if (len > 0) {
        copy = malloc(len);
        if (!copy)
            return EFS_ERR_NOMEM;
        memcpy(copy, buf, len);
    }
    pp = find_slot(m, id);
    if (*pp) {
        free((*pp)->data);
        (*pp)->data = copy;
        (*pp)->len = len;
        (*pp)->has_sum = (sum != NULL);
        if (sum)
            memcpy((*pp)->sum, sum, EFS_HASH_SIZE);
        return EFS_OK;
    }
    r = calloc(1, sizeof(*r));
    if (!r) {
        free(copy);
        return EFS_ERR_NOMEM;
    }
    r->id = *id;
    r->data = copy;
    r->len = len;
    r->has_sum = (sum != NULL);
    if (sum)
        memcpy(r->sum, sum, EFS_HASH_SIZE);
    r->next = NULL;
    *pp = r;
    m->nrec++;
    return EFS_OK;
}

static int mem_del(void *ctx, const struct efs_frag_id *id)
{
    struct mem_store *m = ctx;
    struct mem_rec **pp;
    struct mem_rec *r;

    if (!m || !id)
        return EFS_ERR_INVAL;
    pp = find_slot(m, id);
    if (!*pp)
        return EFS_ERR_NOT_FOUND;
    r = *pp;
    *pp = r->next;
    free(r->data);
    free(r);
    m->nrec--;
    return EFS_OK;
}

static void mem_destroy(void *ctx)
{
    struct mem_store *m = ctx;
    struct mem_rec *r;

    if (!m)
        return;
    r = m->head;
    while (r) {
        struct mem_rec *n = r->next;
        free(r->data);
        free(r);
        r = n;
    }
    free(m);
}

static const struct efs_store_ops mem_ops = {
    .get = mem_get,
    .put = mem_put,
    .del = mem_del,
    .destroy = mem_destroy,
};

struct efs_store *efs_store_mem_create(void)
{
    struct efs_store *s = calloc(1, sizeof(*s));
    struct mem_store *m;

    if (!s)
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m) {
        free(s);
        return NULL;
    }
    m->magic = MEM_STORE_MAGIC;
    s->ops = &mem_ops;
    s->ctx = m;
    return s;
}

void efs_store_mem_free(struct efs_store *s)
{
    if (!s)
        return;
    efs_store_destroy(s);
    free(s);
}

int efs_store_mem_corrupt(struct efs_store *s, const struct efs_frag_id *id)
{
    struct mem_store *m;
    struct mem_rec **pp;

    if (!s || !s->ctx || !id)
        return EFS_ERR_INVAL;
    m = s->ctx;
    if (m->magic != MEM_STORE_MAGIC)
        return EFS_ERR_INVAL;
    pp = find_slot(m, id);
    if (!*pp || !(*pp)->data || (*pp)->len == 0)
        return EFS_ERR_NOT_FOUND;
    (*pp)->data[0] ^= 0x5a;
    return EFS_OK;
}
