#ifndef EFS_PACK_LEDGER_H
#define EFS_PACK_LEDGER_H
#include "efs/packed_segment.h"
#include "efs/opid.h"

/* W125 packed-segment lifetime ledger. Serialized Raft apply verifies the
 * admission and the exact source mapping before planning a switch, and
 * commits all changed mappings and this ledger in ONE atomic KV batch. Load
 * and modify each ledger once per batch, including when several files in
 * that batch share the segment.
 *
 * Every record carries its payload length, so the live and dead byte counts
 * that drive compaction are derived from the record states in the same
 * entry that changes them: they cannot drift from the states. Cancellation
 * (a copy whose switch failed, or an abandoned admission) and release (the
 * file was deleted, overwritten or moved) both make a record dead.
 *
 * ACKs require actual durable storage fences or deletion on the captured
 * member. These planners do not manufacture any of those proofs. */
#define EFS_PACK_PENDING 1u
#define EFS_PACK_LIVE 2u
#define EFS_PACK_RELEASED 3u
#define EFS_PACK_CANCELLED 4u
#define EFS_PACK_ACTIVE 1u
#define EFS_PACK_RECLAIMING 2u
#define EFS_PACK_RETIRED 3u
#define EFS_PACK_LEDGER_VERSION 2u
#define EFS_PACK_LEDGER_F_COLD 1u /* compaction output, never mixed with fresh packing */
#define EFS_PACK_LEDGER_HEADER 160u
#define EFS_PACK_LEDGER_RECORD 37u /* reference digest:32, state:1, len:4 */
#define EFS_PACK_LEDGER_MAX_BYTES (EFS_PACK_LEDGER_HEADER + EFS_PACK_LEDGER_RECORD * EFS_PACK_MAX_RECORDS + EFS_HASH_SIZE)
struct efs_pack_ledger {
    uint64_t segment_id, segment_generation;
    uint32_t geometry, publication_group, count, member_mask;
    uint32_t phase, fence_acks, delete_acks, flags;
    struct efs_opid owner;
    uint8_t body_digest[EFS_HASH_SIZE];
    uint8_t reference_digest[EFS_PACK_MAX_RECORDS][EFS_HASH_SIZE];
    uint8_t state[EFS_PACK_MAX_RECORDS];
    uint32_t len[EFS_PACK_MAX_RECORDS];
};
/* Totals over the record states; payload bytes only (no headers). */
struct efs_pack_ledger_usage {
    uint32_t pending, live, dead;
    uint64_t pending_bytes, live_bytes, dead_bytes, total_bytes;
};
/* References must cover the body's records in canonical offset order. */
int efs_pack_ledger_init(const uint8_t *image, size_t geometry,
    const struct efs_pack_ref *, uint32_t count, const struct efs_opid *,
    uint32_t captured_members, uint32_t flags, struct efs_pack_ledger *);
int efs_pack_ledger_encode(const struct efs_pack_ledger *, uint8_t *, size_t, size_t *);
int efs_pack_ledger_decode(const uint8_t *, size_t, struct efs_pack_ledger *);
void efs_pack_ledger_usage(const struct efs_pack_ledger *, struct efs_pack_ledger_usage *);
/* Exact immutable reference selects a slot. Released or cancelled records
 * never become live again. */
int efs_pack_ledger_slot(const struct efs_pack_ledger *, const struct efs_pack_ref *, unsigned *);
int efs_pack_ledger_publish(struct efs_pack_ledger *, const struct efs_pack_ref *);
int efs_pack_ledger_release(struct efs_pack_ledger *, const struct efs_pack_ref *);
int efs_pack_ledger_cancel(struct efs_pack_ledger *, const struct efs_pack_ref *);
int efs_pack_ledger_reclaim_begin(struct efs_pack_ledger *);
int efs_pack_ledger_fence_ack(struct efs_pack_ledger *, uint32_t member_bit);
int efs_pack_ledger_delete_ack(struct efs_pack_ledger *, uint32_t member_bit);
/* Keep RETIRED state until a durable admission floor fences every replay.
 * Absence is never equivalent to a retired upload permit. */
#endif
