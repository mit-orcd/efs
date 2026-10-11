#ifndef EFS_PACK_MAPPING_H
#define EFS_PACK_MAPPING_H
#include "efs/pack_ledger.h"
#include "efs/meta_apply.h"

/* W95 apply planner. The caller supplies authoritative before/after images
 * already checked by mapping CAS, and commits returned ledger items WITH the
 * mappings and durable receipts in one KV batch. It must hold serialized apply
 * or include the ledger keys in its transaction EXCL guards. Planning neither
 * proves durable PUT nor admits an upload. No mounted writer selects it yet. */
#define EFS_PACK_MAPPING_OBJECT_MAX 64u
#define EFS_PACK_OBJECT_KEY_BYTES 19u
#define EFS_PACK_OBJECT_HEADER 128u
#define EFS_PACK_OBJECT_MAX_BYTES (EFS_PACK_OBJECT_HEADER + EFS_PACK_LEDGER_MAX_BYTES + EFS_HASH_SIZE)
struct efs_pack_object {
    struct efs_pack_ledger ledger;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};
int efs_pack_object_key(uint32_t group, uint64_t id, uint64_t generation,
    uint8_t out[EFS_PACK_OBJECT_KEY_BYTES]);
int efs_pack_object_encode(const struct efs_pack_object *, uint8_t *, size_t, size_t *);
int efs_pack_object_decode(const uint8_t *, size_t, struct efs_pack_object *);
int efs_pack_object_get(struct efs_kv *, uint32_t group, uint64_t id, uint64_t generation,
    struct efs_pack_object *);
struct efs_pack_mapping_change {
    efs_ino_t ino;
    uint64_t inode_generation;
    uint32_t chunk_index;
    const struct efs_meta_chunk *before, *after; /* NULL means absent */
};
struct efs_pack_mapping_plan {
    uint32_t count;
    struct efs_kv_item items[EFS_PACK_MAPPING_OBJECT_MAX];
    void *storage; /* private; release even after aborted commit */
};
int efs_pack_mapping_plan(struct efs_kv *, const struct efs_pack_mapping_change *,
    uint32_t count, struct efs_pack_mapping_plan *);
void efs_pack_mapping_plan_free(struct efs_pack_mapping_plan *);
#endif
