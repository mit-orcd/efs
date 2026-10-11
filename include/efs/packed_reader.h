#ifndef EFS_PACKED_READER_H
#define EFS_PACKED_READER_H
#include "efs/packed_segment.h"
/* Bounded packed-base decoder. Fetch returns exact fragment bytes; every
 * fragment is independently hashed against authoritative metadata. Output is
 * unchanged on failure; successful short records zero-fill the logical chunk.
 * The caller separately applies the captured D25 masks and ordinary deltas. */
typedef int (*efs_packed_fetch_fn)(void *, efs_node_id_t, uint64_t id, uint64_t generation,
    uint32_t fragment_index, uint32_t geometry, uint8_t *out, uint32_t capacity);
int efs_packed_read_base(const struct efs_pack_ref *, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
    const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE], efs_packed_fetch_fn, void *, uint8_t *, uint32_t logical_size);
#endif
