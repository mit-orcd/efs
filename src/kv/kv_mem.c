#include "efs/kv.h"
#include <stdlib.h>
#include <string.h>

#define KV_MEM_MAGIC 0x4B564D31u /* KVM1 */

struct kv_rec {
    uint8_t *key;
    uint32_t klen;
    uint8_t *val;
    uint32_t vlen;
    struct kv_rec *next;
};

struct kv_mem {
    uint32_t magic;
    int fail_next_batch;
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

static void free_list(struct kv_rec *r)
{
    while (r) {
        struct kv_rec *n = r->next;
        free(r->key);
        free(r->val);
        free(r);
        r = n;
    }
}

static int dup_bytes(const uint8_t *src, uint32_t n, uint8_t **out)
{
    uint8_t *p;

    *out = NULL;
    if (n == 0)
        return EFS_OK;
    p = malloc(n);
    if (!p)
        return EFS_ERR_NOMEM;
    memcpy(p, src, n);
    *out = p;
    return EFS_OK;
}

static struct kv_rec *clone_list(const struct kv_rec *src)
{
    struct kv_rec *head = NULL, **pp = &head;

    while (src) {
        struct kv_rec *r = calloc(1, sizeof(*r));
        if (!r) {
            free_list(head);
            return NULL;
        }
        if (dup_bytes(src->key, src->klen, &r->key) != EFS_OK) {
            free(r);
            free_list(head);
            return NULL;
        }
        r->klen = src->klen;
        if (dup_bytes(src->val, src->vlen, &r->val) != EFS_OK) {
            free(r->key);
            free(r);
            free_list(head);
            return NULL;
        }
        r->vlen = src->vlen;
        *pp = r;
        pp = &r->next;
        src = src->next;
    }
    return head;
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
    if (dup_bytes(key, klen, &kcopy) != EFS_OK)
        return EFS_ERR_NOMEM;
    if (dup_bytes(val, vlen, &vcopy) != EFS_OK) {
        free(kcopy);
        return EFS_ERR_NOMEM;
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
    uint32_t need;

    if (!m || !key || klen == 0 || !vlen)
        return EFS_ERR_INVAL;
    pp = find_slot(m, key, klen);
    if (!*pp || key_cmp((*pp)->key, (*pp)->klen, key, klen) != 0)
        return EFS_ERR_NOT_FOUND;
    need = (*pp)->vlen;
    if (*vlen < need || (need > 0 && !val)) {
        *vlen = need;
        return EFS_ERR_INVAL;
    }
    if (need > 0)
        memcpy(val, (*pp)->val, need);
    *vlen = need;
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

static int mem_scan_prefix(void *ctx, const uint8_t *prefix, uint32_t plen,
                           int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                                     const uint8_t *val, uint32_t vlen),
                           void *user)
{
    struct kv_mem *m = ctx;
    struct kv_rec *r;

    if (!m || !cb || (plen > 0 && !prefix))
        return EFS_ERR_INVAL;
    for (r = m->head; r; r = r->next) {
        int past;
        int rc;

        if (plen == 0 || (r->klen >= plen && memcmp(r->key, prefix, plen) == 0)) {
            rc = cb(user, r->key, r->klen, r->val, r->vlen);
            if (rc != 0)
                return rc;
            continue;
        }
        past = key_cmp(r->key, r->klen < plen ? r->klen : plen, prefix, plen);
        if (past > 0)
            break;
    }
    return EFS_OK;
}

static int mem_batch(void *ctx, const struct efs_kv_item *items, uint32_t n)
{
    struct kv_mem *m = ctx;
    struct kv_mem staging;
    struct kv_rec *keep;
    uint32_t i;
    int rc = EFS_OK;

    if (!m || (n > 0 && !items))
        return EFS_ERR_INVAL;
    if (m->fail_next_batch) {
        m->fail_next_batch = 0;
        return EFS_ERR_IO;
    }
    if (n == 0)
        return EFS_OK;
    memset(&staging, 0, sizeof(staging));
    staging.magic = KV_MEM_MAGIC;
    if (m->head) {
        staging.head = clone_list(m->head);
        if (!staging.head)
            return EFS_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        const struct efs_kv_item *it = &items[i];
        if (it->op == EFS_KV_PUT) {
            rc = mem_put(&staging, it->key, it->klen, it->val, it->vlen);
        } else if (it->op == EFS_KV_DEL) {
            rc = mem_del(&staging, it->key, it->klen);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
        } else {
            rc = EFS_ERR_INVAL;
        }
        if (rc != EFS_OK) {
            free_list(staging.head);
            return rc;
        }
    }
    keep = m->head;
    m->head = staging.head;
    free_list(keep);
    return EFS_OK;
}

static void mem_destroy(void *ctx)
{
    struct kv_mem *m = ctx;

    if (!m)
        return;
    free_list(m->head);
    free(m);
}

static const struct efs_kv_ops mem_ops = {
    .put = mem_put,
    .get = mem_get,
    .del = mem_del,
    .scan = mem_scan,
    .scan_prefix = mem_scan_prefix,
    .batch = mem_batch,
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
    m->magic = KV_MEM_MAGIC;
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

int efs_kv_mem_fail_next_batch(struct efs_kv *kv)
{
    struct kv_mem *m;

    if (!kv || !kv->ctx)
        return EFS_ERR_INVAL;
    m = kv->ctx;
    if (m->magic != KV_MEM_MAGIC)
        return EFS_ERR_INVAL;
    m->fail_next_batch = 1;
    return EFS_OK;
}
