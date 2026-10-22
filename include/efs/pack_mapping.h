#ifndef EFS_PACK_MAPPING_H
#define EFS_PACK_MAPPING_H
#include "efs/pack_ledger.h"
#include "efs/meta_apply.h"
#include "efs/metadata.h"
#include "efs/kv_key.h"

/* W125 packed-segment object and mapping planner.
 *
 * A segment object is one KV row (kind PACK_SEGMENT) on its publication
 * group's anchor shard: the lifetime ledger plus the segment's fragment
 * placement and checksums. It is the only owner of the segment's space;
 * inode death never reaches it.
 *
 * The planner keeps ledgers consistent with chunk mappings. The caller
 * supplies authoritative before/after chunk images already checked by its
 * mapping CAS, and commits the returned ledger items WITH the mappings in
 * one KV batch, under serialized apply. A packed reference that disappears
 * (delete, overwrite, truncate, sweep) is released, which makes its bytes
 * dead in the same batch. New packed references are created only by the
 * packer's switch, which publishes ledger records itself after checking
 * the admission; the planner refuses to create one. */
#define EFS_PACK_MAPPING_OBJECT_MAX 64u
#define EFS_PACK_OBJECT_KEY_BYTES 19u
#define EFS_PACK_OBJECT_HEADER 128u
#define EFS_PACK_OBJECT_MAX_BYTES (EFS_PACK_OBJECT_HEADER + EFS_PACK_LEDGER_MAX_BYTES + EFS_HASH_SIZE)
struct efs_pack_object {
    struct efs_pack_ledger ledger;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};
/* Anchor shard of a publication group: 1 for group 0, 2 for group 2. */
uint32_t efs_pack_group_anchor(uint32_t group);
int efs_pack_object_key(uint32_t group, uint64_t id, uint64_t generation,
    uint8_t out[EFS_PACK_OBJECT_KEY_BYTES]);
int efs_pack_object_encode(const struct efs_pack_object *, uint8_t *, size_t, size_t *);
int efs_pack_object_decode(const uint8_t *, size_t, struct efs_pack_object *);
int efs_pack_object_get(struct efs_kv *, uint32_t group, uint64_t id, uint64_t generation,
    struct efs_pack_object *);
/* Abandoned admissions (packer step 1): ACTIVE segment objects with
 * PENDING records whose owner is not `me` (this node, this term). One call
 * examines at most `limit` keys of the group's object prefix, starting just
 * after scan->cursor, and resumes there next call; reaching the end resets
 * the cursor, so every object is reached within ceil(objects/limit) calls.
 * Phase, record counts and owner are read from the encoded object before a
 * full decode, which runs (outside the scan) only for those candidates; an
 * object that fails the full decode is counted as refused, never cancelled.
 * At most `max` (id, generation) pairs are returned in *n. A hint only:
 * the CANCEL apply revalidates. */
struct efs_pack_abandon_scan {
    uint8_t cursor[EFS_KV_KEY_MAX];
    uint32_t cursor_len; /* 0: from the start */
    uint64_t scanned, actionable, refused, wraps; /* totals over calls */
};
int efs_pack_abandon_scan(struct efs_kv *, uint32_t group, const struct efs_opid *me,
    struct efs_pack_abandon_scan *, uint32_t limit, uint64_t *ids, uint64_t *gens,
    uint32_t max, uint32_t *n);
/* A packed chunk row's reference must name exactly this file, chunk zero,
 * its record generation and content epoch, and the file's own group. */
int efs_pack_chunk_identity(const struct efs_meta_chunk *, efs_ino_t ino,
    uint64_t inode_generation, uint32_t chunk_index);
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

/* Background packer metadata (W125), applied by the segment's publication
 * group under serialized apply.
 *
 * ADMIT records a durable segment object whose records are all PENDING,
 * before any fragment is written, so a crashed copy is always found and
 * reclaimed (I15). Re-admitting the identical object is a no-op.
 *
 * SWITCH moves up to EFS_PACK_SWITCH_MAX files into the admitted segment in
 * one atomic batch. Each member names the exact source it was copied from:
 * the digest of the file's chunk-zero row as stored (base, packed reference
 * and spans). A member switches only if the inode is the same generation,
 * still linked, has no lease, and its row still hashes to that digest; the
 * new packed reference must carry the row's newest content epoch and a new
 * record generation. Its ledger record becomes LIVE, the old per-file
 * fragments become GC records, and an old packed record is released. Any
 * other member is CANCELLED, so its bytes are dead space in the segment.
 *
 * CANCEL abandons an admitted segment's remaining PENDING records (a packer
 * that lost leadership or failed its copy). */
#define EFS_MD_PACK_SWITCH_MEMBER_BYTES (8u + 8u + EFS_HASH_SIZE + EFS_PACK_REF_BYTES)
#define EFS_PACK_SWITCH_MAX 128u
struct efs_pack_switch_member {
    efs_ino_t ino;
    uint64_t inode_generation;
    uint8_t source_digest[EFS_HASH_SIZE];
    struct efs_pack_ref ref;
};
struct efs_pack_switch {
    uint32_t group;
    uint64_t segment_id, segment_generation;
    uint32_t count;
    struct efs_pack_switch_member members[EFS_PACK_SWITCH_MAX];
};
/* Digest of a file's chunk-zero row exactly as stored, its decoded base and
 * newest content epoch (base or span). NOT_FOUND when the row is absent. */
int efs_meta_pack_source(struct efs_kv *, efs_ino_t ino, uint64_t inode_generation,
    uint8_t digest[EFS_HASH_SIZE], struct efs_meta_chunk *base, uint64_t *content_epoch);
int efs_meta_apply_pack_admit(struct efs_kv *, uint32_t group, const struct efs_pack_object *);
/* *switched (optional) counts the members that became LIVE. */
int efs_meta_apply_pack_switch(struct efs_kv *, const struct efs_pack_switch *, uint32_t *switched);
int efs_meta_apply_pack_cancel(struct efs_kv *, uint32_t group, uint64_t id, uint64_t generation);
/* RETIRE forgets segments that are no longer ACTIVE once every member's
 * reclaim journal has durably accepted them (W124 journal, kind SEGMENT). */
#define EFS_PACK_RETIRE_MAX 256u
int efs_meta_apply_pack_retire(struct efs_kv *, uint32_t group, const uint64_t *ids,
                               const uint64_t *gens, uint32_t n, uint32_t *retired);

/* Packing candidates. A publication of chunk zero that leaves a file of at
 * most EFS_PACK_THRESHOLD bytes records the file in its group's candidate
 * index, in the same batch: key [anchor:2][kind:1][epoch:8][ino:8], value
 * [inode generation:8], epoch = leader time / EFS_PACK_EPOCH_S. Iterating
 * the index visits files by creation epoch, which the packer then groups
 * by directory. Entries are hints: the packer re-validates each file and
 * UNINDEX removes packed, stale and refused ones. The epoch length is part
 * of the key format, never a runtime setting. */
#define EFS_PACK_THRESHOLD 16384u
/* 60 s: the packer scans the oldest epochs first, so an epoch must be short
 * against the minimum age or a sustained writer's young files (ordered by
 * inode inside one epoch) crowd the eligible ones out of the scan window. */
#define EFS_PACK_EPOCH_S 60u
#define EFS_PACK_CANDIDATE_KEY_BYTES 19u
#define EFS_PACK_UNINDEX_MAX 512u
int efs_pack_candidate_key(uint32_t anchor, uint64_t epoch, efs_ino_t ino,
    uint8_t out[EFS_PACK_CANDIDATE_KEY_BYTES]);
/* A candidate as the packer sees it on its local replica (not authority:
 * the switch re-checks the exact source). STALE when the generation does
 * not match or the inode is gone. */
struct efs_pack_candidate {
    efs_ino_t ino, parent;
    uint64_t inode_generation, size, mtime, content_epoch;
    uint32_t mode, nlink, ndelta;
    uint64_t active_lanes;
    struct efs_meta_chunk base;
    /* D25: bytes of the base still visible (clip and fence history); the
     * deltas below are already clipped to their visible lengths. The image
     * is masked; content_epoch stays the newest source-part epoch. */
    uint32_t base_visible;
    struct efs_meta_delta deltas[EFS_CHUNK_DELTA_MAX]; /* ndelta spans, apply order */
    uint8_t source_digest[EFS_HASH_SIZE];
};
int efs_meta_pack_candidate(struct efs_kv *, efs_ino_t ino, uint64_t inode_generation,
    struct efs_pack_candidate *);
int efs_meta_apply_pack_unindex(struct efs_kv *, uint32_t group, const uint64_t *epochs,
    const efs_ino_t *inos, uint32_t n);
/* Backfill of stores written before the packer: INDEX adds candidates
 * found by the leader's scan and advances the group's backfill cursor,
 * kept at the reserved candidate key (epoch UINT64_MAX, inode 1) as
 * [next shard:4][done:1]. Candidate scans skip that key. */
#define EFS_PACK_CURSOR_EPOCH UINT64_MAX
#define EFS_PACK_INDEX_MAX 512u
struct efs_pack_index_item { uint64_t epoch; efs_ino_t ino; uint64_t inode_generation; };
int efs_meta_apply_pack_index(struct efs_kv *, uint32_t group, uint32_t next_shard, int done,
    const struct efs_pack_index_item *, uint32_t n);
/* 0 and *next_shard 0 when no cursor exists yet. */
int efs_meta_pack_backfill_cursor(struct efs_kv *, uint32_t group, uint32_t *next_shard, int *done);

/* Raft command payloads, after the command byte:
 *   ADMIT  [group:1][object]
 *   SWITCH [group:1][id:8][generation:8][count:2] then count members of
 *          [ino:8][inode generation:8][source digest:32][reference:96]
 *   CANCEL [group:1][id:8][generation:8]
 *   RETIRE [group:1][count:2] then count [id:8][generation:8]
 *   UNINDEX [group:1][count:2] then count [epoch:8][ino:8]
 *   INDEX  [group:1][count:2][next shard:4][done:1] then count
 *          [epoch:8][ino:8][inode generation:8] */
int efs_pack_switch_encode(const struct efs_pack_switch *, uint8_t *, size_t, size_t *);
int efs_pack_switch_decode(const uint8_t *, size_t, struct efs_pack_switch *);
#endif
