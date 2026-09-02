#ifndef EFS_META_APPLY_H
#define EFS_META_APPLY_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/opid.h"

/* Applied-state SM over the ordered KV (architecture.md §5 / §10 step 3).
 * CREATE file = one atomic batch {dentry, inode, alloc} on the parent shard
 * (co-location: inode_shard(new) = inode_shard(parent)). LOOKUP reads the
 * dentry projection. A dentry whose inode row cannot be resolved is I9
 * (EFS_ERR_IO), never a fake absence. */

#define EFS_META_LANES 64
#define EFS_META_INO_BYTES   80
#define EFS_META_DENT_BYTES  20
#define EFS_META_ALLOC_BYTES 8

struct efs_meta_row {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t mode;
    uint32_t nlink;
    efs_ino_t parent;
    uint64_t base_size;
    uint64_t active_lanes;
};

struct efs_meta_dentry {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t type;
};

struct efs_meta_chunk {
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

int efs_meta_apply_init(struct efs_kv *kv);
int efs_meta_apply_get_inode(struct efs_kv *kv, efs_ino_t ino,
                             struct efs_meta_row *out);
int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out);
/* LOOKUP + inode fetch. Dentry miss = NOT_FOUND; inode miss = I9 (IO). */
int efs_meta_apply_resolve(struct efs_kv *kv, efs_ino_t parent, const char *name,
                           struct efs_meta_dentry *dent, struct efs_meta_row *row);
int efs_meta_apply_create_file(struct efs_kv *kv, efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out);
int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out);
int efs_meta_apply_unlink(struct efs_kv *kv, efs_ino_t parent, const char *name);
int efs_meta_apply_publish(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                           uint64_t new_size, const struct efs_meta_chunk *ch);
int efs_meta_apply_get_chunk(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                             struct efs_meta_chunk *out);
int efs_meta_apply_check(struct efs_kv *kv);
int efs_meta_pack_inode(const struct efs_meta_row *r, uint8_t *out, uint32_t cap);
int efs_meta_pack_dentry(const struct efs_meta_dentry *d, uint8_t *out,
                         uint32_t cap);
int efs_meta_apply_peek_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next);

#endif
