#ifndef EFS_STORE_H
#define EFS_STORE_H

#include "efs/common.h"
#include <stdint.h>

/* Chunk-fragment store. Production NVMe lives in src/server/store.c
 * (server_read_fragment / server_write_fragment_*); this vtable is the
 * seam the simulator implements with a fault-injecting in-memory disk,
 * and that later handler dispatch will call without knowing the backend.
 *
 * Simulator identity is FileID-scoped (architecture.md §7.3):
 * (export, ino, inode_generation, chunk_index, chunk_generation,
 * fragment_index, coding_profile_id). Production NVMe still keys
 * (ino, chunk_index, fragment_index) until efsd migrates; extra fields
 * zero-init and are ignored there. */

struct efs_frag_id {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint64_t inode_generation;
    uint64_t chunk_generation;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint32_t coding_profile_id;
};

struct efs_store_ops {
    /* *len is capacity in, bytes out. sum / sum_ok may be NULL.
     * *sum_ok is set when a sidecar checksum was stored with the put. */
    int (*get)(void *ctx, const struct efs_frag_id *id,
               uint8_t *buf, uint32_t *len,
               uint8_t sum[EFS_HASH_SIZE], int *sum_ok);
    /* sum may be NULL (no sidecar). */
    int (*put)(void *ctx, const struct efs_frag_id *id,
               const uint8_t *buf, uint32_t len,
               const uint8_t sum[EFS_HASH_SIZE]);
    int (*del)(void *ctx, const struct efs_frag_id *id);
    void (*destroy)(void *ctx);
};

struct efs_store {
    const struct efs_store_ops *ops;
    void *ctx;
};

static inline int efs_store_get(struct efs_store *s, const struct efs_frag_id *id,
                                uint8_t *buf, uint32_t *len,
                                uint8_t sum[EFS_HASH_SIZE], int *sum_ok)
{
    if (!s || !s->ops || !s->ops->get)
        return EFS_ERR_INVAL;
    return s->ops->get(s->ctx, id, buf, len, sum, sum_ok);
}

static inline int efs_store_put(struct efs_store *s, const struct efs_frag_id *id,
                                const uint8_t *buf, uint32_t len,
                                const uint8_t sum[EFS_HASH_SIZE])
{
    if (!s || !s->ops || !s->ops->put)
        return EFS_ERR_INVAL;
    return s->ops->put(s->ctx, id, buf, len, sum);
}

static inline int efs_store_del(struct efs_store *s, const struct efs_frag_id *id)
{
    if (!s || !s->ops || !s->ops->del)
        return EFS_ERR_INVAL;
    return s->ops->del(s->ctx, id);
}

static inline void efs_store_destroy(struct efs_store *s)
{
    if (!s)
        return;
    if (s->ops && s->ops->destroy)
        s->ops->destroy(s->ctx);
    s->ops = NULL;
    s->ctx = NULL;
}

/* In-memory backend (simulator / unit tests). No sockets, no NVMe. */
struct efs_store *efs_store_mem_create(void);
void efs_store_mem_free(struct efs_store *s);
/* Flip one payload byte; leave the sidecar checksum unchanged (I25). */
int efs_store_mem_corrupt(struct efs_store *s, const struct efs_frag_id *id);

#endif
