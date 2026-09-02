#include "efs/kv.h"
#include <stdlib.h>
#include <string.h>

struct kv_rec {
    uint8_t *key;
    uint32_t klen;
    uint8_t *val;
    uint32_t vlen;
    struct kv_rec *next;
};

struct kv_mem {
    struct kv_rec *head;
};

static int key_cmp(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl)
{
    uint32_t n = al < bl ? al : bl;
    int c = n ? memcmp(a, b, n) : 0;
    if (c != 0)
        return c;
    if (al < bl)
        return -1;
    if (al > bl)
        return 1;
    return 0;
}

static struct kv_rec **find_slot(struct kv_mem *m, const uint8_t *key, uint32_t klen)
{
    struct kv_rec **pp = &m->head;
    while (*pp) {
        int c = key_cmp((*pp)->key, (*pp)->klen, key, klen);
        if (c >= 0)
            return pp;
        pp = &(*pp)->next;
    }
    return pp;
}

static int mem_put(void *ctx, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct kv_mem *m = ctx;
    struct kv_rec **pp;
    struct kv_rec *r;
    uint8_t *kcopy = NULL, *vcopy = NULL;

    if (!m || !key || klen == 0 || (vlen > 0 && !val))
        return EFS_ERR_INVAL;
    kcopy = malloc(klen);
    if (!kcopy)
        return EFS_ERR_NOMEM;
    memcpy(kcopy, key, klen);
    if (vlen > 0) {
        vcopy = malloc(vlen);
        if (!vcopy) {
            free(kcopy);
            return EFS_ERR_NOMEM;
        }
        memcpy(vcopy, val, vlen);
    }
    pp = find_slot(m, key, klen);
    if (*pp && key_cmp((*pp)->key, (*pp)->klen, key, klen) == 0) {
        free((*pp)->key);
        free((*pp)->val);
        (*pp)->key = kcopy;
        (*pp)->klen = klen;
        (*pp)->val = vcopy;
        (*pp)->vlen = vlen;
        return EFS_OK;
    }
    r = calloc(1, sizeof(*r));
    if (!r) {
        free(kcopy);
        free(vcopy);
        return EFS_ERR_NOMEM;
    }
    r->key = kcopy;
    r->klen = klen;
    r->val = vcopy;
    r->vlen = vlen;
    r->next = *pp;
    *pp = r;
    return EFS_OK;
}

static int mem_get(void *ctx, const uint8_t *key, uint32_t klen,
                   uint8_t *val, uint32_t *vlen)
{
    struct kv_mem *m = ctx;
    struct kv_rec **pp;

    if (!m || !key || klen == 0 || !vlen)
        return EFS_ERR_INVAL;
    pp = find_slot(m, key, klen);
    if (!*pp || key_cmp((*pp)->key, (*pp)->klen, key, klen) != 0)
        return EFS_ERR_NOT_FOUND;
    if (*vlen < (*pp)->vlen)
        return EFS_ERR_INVAL;
    if ((*pp)->vlen > 0 && !val)
        return EFS_ERR_INVAL;
    if ((*pp)->vlen > 0)
        memcpy(val, (*pp)->val, (*pp)->vlen);
    *vlen = (*pp)->vlen;
    return EFS_OK;
}

static int mem_del(void *ctx, const uint8_t *key, uint32_t klen)
{
    struct kv_mem *m = ctx;
    struct kv_rec **pp;
    struct kv_rec *r;

    if (!m || !key || klen == 0)
        return EFS_ERR_INVAL;
    pp = find_slot(m, key, klen);
    if (!*pp || key_cmp((*pp)->key, (*pp)->klen, key, klen) != 0)
        return EFS_ERR_NOT_FOUND;
    r = *pp;
    *pp = r->next;
    free(r->key);
    free(r->val);
    free(r);
    return EFS_OK;
}

static int mem_scan(void *ctx,
                    int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                              const uint8_t *val, uint32_t vlen),
                    void *user)
{
    struct kv_mem *m = ctx;
    struct kv_rec *r;

    if (!m || !cb)
        return EFS_ERR_INVAL;
    for (r = m->head; r; r = r->next) {
        int rc = cb(user, r->key, r->klen, r->val, r->vlen);
        if (rc != 0)
            return rc;
    }
    return EFS_OK;
}

static void mem_destroy(void *ctx)
{
    struct kv_mem *m = ctx;
    struct kv_rec *r;

    if (!m)
        return;
    r = m->head;
    while (r) {
        struct kv_rec *n = r->next;
        free(r->key);
        free(r->val);
        free(r);
        r = n;
    }
    free(m);
}

static const struct efs_kv_ops mem_ops = {
    .put = mem_put,
    .get = mem_get,
    .del = mem_del,
    .scan = mem_scan,
    .destroy = mem_destroy,
};

struct efs_kv *efs_kv_mem_create(void)
{
    struct efs_kv *kv = calloc(1, sizeof(*kv));
    struct kv_mem *m;

    if (!kv)
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m) {
        free(kv);
        return NULL;
    }
    kv->ops = &mem_ops;
    kv->ctx = m;
    return kv;
}

void efs_kv_mem_free(struct efs_kv *kv)
{
    if (!kv)
        return;
    efs_kv_destroy(kv);
    free(kv);
}
