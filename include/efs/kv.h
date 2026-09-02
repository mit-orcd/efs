#ifndef EFS_KV_H
#define EFS_KV_H

#include "efs/common.h"
#include <stdint.h>

/* Ordered key-value seam. Production metadata still serializes to the
 * EFSM blob + CoW pages (src/meta/metadata.c); this vtable is what the
 * simulator implements with a fault-injecting in-memory disk, and what
 * flush/rebuild will call once handler dispatch is carved (Phase M step 4).
 *
 * Keys and values are opaque byte strings. scan visits in lexicographic
 * key order. */

struct efs_kv_ops {
    int (*put)(void *ctx, const uint8_t *key, uint32_t klen,
               const uint8_t *val, uint32_t vlen);
    int (*get)(void *ctx, const uint8_t *key, uint32_t klen,
               uint8_t *val, uint32_t *vlen);
    int (*del)(void *ctx, const uint8_t *key, uint32_t klen);
    /* cb returns nonzero to stop. Returns EFS_OK or the last cb rc. */
    int (*scan)(void *ctx,
                int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                          const uint8_t *val, uint32_t vlen),
                void *user);
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

#endif
