#ifndef EFS_PACK_LEDGER_H
#define EFS_PACK_LEDGER_H
#include "efs/packed_segment.h"
#include "efs/opid.h"

/* W95 shared-object lifetime planner. Private Raft/RPC gates select this
 * format; the mounted writer remains gated pending cache ownership and GC. Serialized Raft apply must verify upload/session authority,
 * durable receipts and mapping CAS before planning publication; retirement
 * requires the exact current mapping. Apply all changed mappings, receipts
 * and this ledger in ONE atomic KV batch. Load/modify each ledger once per
 * batch, including when several files in that batch share the object.
 *
 * Cancellation requires authoritative session revocation, not elapsed time.
 * ACKs require actual durable storage fences/deletion on the captured member.
 * These internal planners do not manufacture any of those authority proofs. */
#define EFS_PACK_PENDING 1u
#define EFS_PACK_LIVE 2u
#define EFS_PACK_RELEASED 3u
#define EFS_PACK_CANCELLED 4u
#define EFS_PACK_ACTIVE 1u
#define EFS_PACK_RECLAIMING 2u
#define EFS_PACK_RETIRED 3u
#define EFS_PACK_LEDGER_HEADER 128u
#define EFS_PACK_LEDGER_MAX_BYTES (EFS_PACK_LEDGER_HEADER + 33u * EFS_PACK_MAX_RECORDS + EFS_HASH_SIZE)
struct efs_pack_ledger {
    uint64_t segment_id, segment_generation;
    uint32_t geometry, publication_group, count, member_mask;
    uint32_t phase, fence_acks, delete_acks;
    struct efs_opid owner;
    uint8_t body_digest[EFS_HASH_SIZE];
    uint8_t reference_digest[EFS_PACK_MAX_RECORDS][EFS_HASH_SIZE];
    uint8_t state[EFS_PACK_MAX_RECORDS];
};
/* References must cover the body's records in canonical offset order. */
int efs_pack_ledger_init(const uint8_t *image, size_t geometry,
    const struct efs_pack_ref *, uint32_t count, const struct efs_opid *,
    uint32_t captured_members, struct efs_pack_ledger *);
int efs_pack_ledger_encode(const struct efs_pack_ledger *, uint8_t *, size_t, size_t *);
int efs_pack_ledger_decode(const uint8_t *, size_t, struct efs_pack_ledger *);
/* Exact immutable reference selects a slot. Released/cancelled publication
 * retries fail stale; historical successful receipts never resurrect a slot. */
int efs_pack_ledger_publish(struct efs_pack_ledger *, const struct efs_pack_ref *);
int efs_pack_ledger_release(struct efs_pack_ledger *, const struct efs_pack_ref *);
int efs_pack_ledger_cancel(struct efs_pack_ledger *, const struct efs_pack_ref *);
int efs_pack_ledger_reclaim_begin(struct efs_pack_ledger *);
int efs_pack_ledger_fence_ack(struct efs_pack_ledger *, uint32_t member_bit);
int efs_pack_ledger_delete_ack(struct efs_pack_ledger *, uint32_t member_bit);
/* Keep RETIRED state until a durable admission floor fences every replay.
 * Absence is never equivalent to a retired upload permit. */
#endif
