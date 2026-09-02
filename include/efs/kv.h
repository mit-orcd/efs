#ifndef EFS_KV_H
#define EFS_KV_H

#include "efs/common.h"
#include <stdint.h>

/* Ordered applied-state KV (architecture.md §5 / §10 step 3).
 * Keys and values are opaque byte strings. scan visits lexicographic order.
 * A batch of put/del is atomic: all items apply or the store is unchanged.
 * RAM is a cache; a miss that cannot be resolved is a resource failure (I9),
 * never "absent".
 *
 * Production efsd still mutates the in-memory efs_export table until Raft
 * apply (step 4) writes this KV. The simulator's metadata path uses this
 * as the store. */

#define EFS_KV_PUT 1
#define EFS_KV_DEL 2

struct efs_kv_item {
    uint8_t op; /* EFS_KV_PUT or EFS_KV_DEL */
    const uint8_t *key;
    uint32_t klen;
    const uint8_t *val; /* PUT only */
    uint32_t vlen;
};

struct efs_kv_ops {
    int (*put)(void *ctx, const uint8_t *key, uint32_t klen,
               const uint8_t *val, uint32_t vlen);
    /* On a short buffer the key is found: *vlen is set to the stored size
     * and EFS_ERR_INVAL is returned (probe with *vlen = 0). */
    int (*get)(void *ctx, const uint8_t *key, uint32_t klen,
               uint8_t *val, uint32_t *vlen);
    int (*del)(void *ctx, const uint8_t *key, uint32_t klen);
    int (*scan)(void *ctx,
                int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                          const uint8_t *val, uint32_t vlen),
                void *user);
    int (*scan_prefix)(void *ctx, const uint8_t *prefix, uint32_t plen,
                       int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                                 const uint8_t *val, uint32_t vlen),
                       void *user);
    int (*batch)(void *ctx, const struct efs_kv_item *items, uint32_t n);
    void (*destroy)(void *ctx);
};

struct efs_kv {
    const struct efs_kv_ops *ops;
    void *ctx;
};

static inline int efs_kv_put(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                             const uint8_t *val, uint32_t vlen)
{
    if (!kv || !kv->ops || !kv->ops->put)
        return EFS_ERR_INVAL;
    return kv->ops->put(kv->ctx, key, klen, val, vlen);
}

static inline int efs_kv_get(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                             uint8_t *val, uint32_t *vlen)
{
    if (!kv || !kv->ops || !kv->ops->get)
        return EFS_ERR_INVAL;
    return kv->ops->get(kv->ctx, key, klen, val, vlen);
}

static inline int efs_kv_del(struct efs_kv *kv, const uint8_t *key, uint32_t klen)
{
    if (!kv || !kv->ops || !kv->ops->del)
        return EFS_ERR_INVAL;
    return kv->ops->del(kv->ctx, key, klen);
}

static inline int efs_kv_scan(struct efs_kv *kv,
                              int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                                        const uint8_t *val, uint32_t vlen),
                              void *user)
{
    if (!kv || !kv->ops || !kv->ops->scan)
        return EFS_ERR_INVAL;
    return kv->ops->scan(kv->ctx, cb, user);
}

static inline int efs_kv_scan_prefix(struct efs_kv *kv,
                                     const uint8_t *prefix, uint32_t plen,
                                     int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                                               const uint8_t *val, uint32_t vlen),
                                     void *user)
{
    if (!kv || !kv->ops || !kv->ops->scan_prefix)
        return EFS_ERR_INVAL;
    return kv->ops->scan_prefix(kv->ctx, prefix, plen, cb, user);
}

static inline int efs_kv_batch(struct efs_kv *kv, const struct efs_kv_item *items,
                               uint32_t n)
{
    if (!kv || !kv->ops || !kv->ops->batch)
        return EFS_ERR_INVAL;
    return kv->ops->batch(kv->ctx, items, n);
}

static inline void efs_kv_destroy(struct efs_kv *kv)
{
    if (!kv)
        return;
    if (kv->ops && kv->ops->destroy)
        kv->ops->destroy(kv->ctx);
    kv->ops = NULL;
    kv->ctx = NULL;
}

struct efs_kv *efs_kv_mem_create(void);
void efs_kv_mem_free(struct efs_kv *kv);
/* Next batch returns EFS_ERR_IO and does not mutate (atomicity test). */
int efs_kv_mem_fail_next_batch(struct efs_kv *kv);

#endif
