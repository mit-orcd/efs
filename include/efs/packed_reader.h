#ifndef EFS_PACKED_READER_H
#define EFS_PACKED_READER_H
#include "efs/packed_segment.h"
/* Bounded packed-base decoder (W125 client reads). Fetch returns exact fragment
 * bytes, hashed once and verified against BOTH authorities: the transport
 * reply's digest (the bytes arrived as the sender built them) and
 * expected_sum, the mapping's committed fragment digest (they are the right
 * bytes). The reader never rehashes. Output is unchanged on failure;
 * successful short records zero-fill the logical chunk. The caller separately
 * applies the captured D25 masks and ordinary deltas.
 *
 * On EFS_OK exactly `capacity` (= geometry/2) verified bytes sit at *fragp:
 * in the caller's scratch `out` when *ownedp is NULL, or inside the
 * malloc-owned *ownedp buffer otherwise (normal free() ownership; *fragp may
 * point anywhere inside it). On failure *ownedp is NULL and out is undefined. */
typedef int (*efs_packed_fetch_fn)(void *, efs_node_id_t, uint64_t id, uint64_t generation,
    uint32_t fragment_index, uint32_t geometry, uint8_t *out, uint32_t capacity,
    const uint8_t *expected_sum, const uint8_t **fragp, uint8_t **ownedp);
/* The single fragment hash checked against both authorities. *which names the
 * failing one on EFS_ERR_CHECKSUM: 1 = reply digest, 2 = mapping digest. */
int efs_packed_fragment_verify(const uint8_t *frag, uint32_t len,
    const uint8_t *reply_digest, const uint8_t *mapping_sum, int *which);
/* Zero means invalid reference. The scratch variant never allocates: callers
 * charge this capacity to their memory budget before issuing any fetch and
 * retain exclusive ownership until it returns. Scratch and output are disjoint. */
size_t efs_packed_read_scratch_size(const struct efs_pack_ref *);
int efs_packed_read_base_scratch(const struct efs_pack_ref *, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
    const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE], efs_packed_fetch_fn, void *,
    uint8_t *, uint32_t logical_size, void *scratch, size_t capacity);
/* Convenience wrapper for standalone tools; mounted reads use charged scratch. */
int efs_packed_read_base(const struct efs_pack_ref *, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
    const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE], efs_packed_fetch_fn, void *, uint8_t *, uint32_t logical_size);
#endif
