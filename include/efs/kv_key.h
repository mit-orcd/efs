#ifndef EFS_KV_KEY_H
#define EFS_KV_KEY_H

#include "efs/common.h"
#include "efs/opid.h"

/* Byte encoding of architecture.md §5 keys. Shard prefix multiplexes
 * isolated namespaces into one engine. Big-endian so range scans match
 * integer order. Kind bytes keep tuple types from colliding. */

#define EFS_KV_SHARD_BITS 12
#define EFS_KV_SHARD_MASK 0xFFFu
#define EFS_KV_KEY_MAX    320
#define EFS_KV_KIND_EXPIRY_POLICY 42 /* directory FileID: operator policy */
#define EFS_KV_KIND_EXPIRY 43 /* FileID: fixed deadline and monotonic revision */
#define EFS_KV_KIND_EXPIRY_INDEX 44 /* group anchor/deadline/FileID: revision */
#define EFS_KV_KIND_PACK_SEGMENT 45 /* group anchor/segment id/generation: W125 packed segment */
#define EFS_KV_KIND_PACK_CANDIDATE 46 /* group anchor/epoch/FileID: W125 packing candidate */
#define EFS_KV_KIND_TRUNCATE_RECEIPT 47 /* inode/session: D25 truncate opid window */
#define EFS_KV_KIND_TRUNC_PENDING 48 /* inode anchor/ino: D25 history awaiting sweep */
#define EFS_KV_KIND_DEGRADED 51 /* group anchor/FileID/object/chunk: W154 stripe awaiting rebuild */
/* 36-41 are reserved: never reuse. Snapshot import refuses them. */
#define EFS_KV_KIND_RESERVED_MIN 36
#define EFS_KV_KIND_RESERVED_MAX 41
#define EFS_KV_KIND_QUERY_POLICY 35 /* root export: version:8, mask:4 */
#define EFS_KV_KIND_CLOSE_STEP 34 /* staged replay-identified bounded close obligation */
#define EFS_KV_KIND_CLOSE_CLEANUP 33 /* durable deferred final-close effects */
#define EFS_KV_KIND_LEASE_EDGE 32 /* bounded staged session/shard lease replay */

#define EFS_KV_KIND_APPEND_RECEIPT 31 /* inode/session: bounded reservation results */
#define EFS_KV_KIND_PUT_TICKET 29 /* admitted immutable body, session-owned */
#define EFS_KV_KIND_PUT_FLOOR 30 /* retired ticket sequence; durable replay fence */
#define EFS_KV_KIND_PUT_ORPHAN 53 /* retired REJECTED ticket: body awaiting revocation-gated collection */
#define EFS_KV_KIND_PUT_COUNT 54 /* live tickets in one session/lane-shard stream (admission bound) */
#define EFS_KV_KIND_PUT_STREAM 250 /* group anchor/session/epoch/lane shard: D31 live ticket stream index */
#define EFS_KV_KIND_MEMBERSHIP 55 /* group anchor: W153 committed formation membership */
#define EFS_KV_KIND_ORPHAN 28 /* zero-link inode awaiting lease/txn-safe reclamation */
#define EFS_KV_KIND_PUBLICATION_FLOOR 27 /* stream replay barrier, never expires */
#define EFS_KV_KIND_PUBLICATION 26 /* FileID/chunk/intent digest: durable verdict */

#define EFS_KV_KIND_ALLOC    1
#define EFS_KV_KIND_INODE    2
#define EFS_KV_KIND_DENTRY   3
#define EFS_KV_KIND_CHUNK    4
#define EFS_KV_KIND_LANE     5
#define EFS_KV_KIND_OPID     6
#define EFS_KV_KIND_VER      7  /* sidecar version of an exclusive key */
#define EFS_KV_KIND_INTENT   8  /* exclusive intent, keyed by the data key */
#define EFS_KV_KIND_GUARD    9  /* shared predicate guard + txid suffix */
#define EFS_KV_KIND_REDUCE   10 /* pending commutative reduction + txid */
#define EFS_KV_KIND_DECISION 11 /* live coordinator decision: verdict + participants */
#define EFS_KV_KIND_DECISION_RETIRED 49 /* retired decision: verdict + retire time */
#define EFS_KV_KIND_DECISION_CURSOR 50 /* per-shard retired-decision reclaim cursor */
/* W77 staged publication: a chunk's prepared publication (lane shard, chunk
 * key layout) or a FileID truncate pin (inode shard, lane 0xFF + txid). */
#define EFS_KV_KIND_PUB_STAGE 52 /* W77/K14 staged publication; 51 is W154 DEGRADED */
#define EFS_KV_KIND_DSEQ     12 /* per-(dir, dentry-shard) emptiness witness */
#define EFS_KV_KIND_SESSION  13 /* session record on hash(uuid) shard */
#define EFS_KV_KIND_SESS_LOCAL 14 /* per-shard established epoch + fence */
#define EFS_KV_KIND_LEASE    15 /* open lease (FileID, session) on inode shard */
#define EFS_KV_KIND_LOCK     16 /* POSIX lock record on inode shard */
#define EFS_KV_KIND_APPEND_CUR 17 /* FileID append cursor (watermark/frontier) */
#define EFS_KV_KIND_APPEND_RSV 18 /* FileID append reservation at offset */
#define EFS_KV_KIND_PVER     19 /* directory parent_version sidecar */
#define EFS_KV_KIND_EXPORT   20 /* per-export salt (chosen at mkfs, §7.4) */
#define EFS_KV_KIND_GC       21 /* dead fragment set + per-fragment acks (L7) */
#define EFS_KV_KIND_REAP     22 /* dead inode awaiting lane sweep + frag GC */
#define EFS_KV_KIND_XATTR    23 /* one extended-attribute blob per inode */
#define EFS_KV_KIND_LANE_AUTHORITY 25 /* FileID/lane: D25 admission stamp + geometry */
#define EFS_KV_KIND_CONTENT_FENCE 24 /* FileID + authority: durable D25 history */

/* GC and REAP records for every shard of a group live on that group's one
 * fixed anchor shard, so the background reaper scans ONE prefix per group
 * instead of 2048. The anchor must stay on the writer entry's own group
 * (a group's log may only write its own shards' keys): group 0 owns the odd
 * shards, group 2 the even ones — same parity rule as
 * efs_raft_shard_group(), which this must never drift from. */
static inline uint32_t efs_kv_anchor_shard(uint32_t shard)
{
    return (shard & 1u) ? 1u : 2u;
}

static inline uint32_t efs_kv_inode_shard(efs_ino_t ino)
{
    return (uint32_t)(ino & EFS_KV_SHARD_MASK);
}

uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN]);
/* lane = ci % 64; lane 0 is the inode shard (architecture.md §7.3). */
uint32_t efs_kv_lane_shard(efs_ino_t ino, uint8_t lane);
/* dir_lane = hash(name)%64; layout 0 = LOCAL (parent shard). */
uint8_t efs_kv_dir_lane(const char *name);
uint32_t efs_kv_dentry_shard(efs_ino_t parent, const char *name, uint8_t layout);

int efs_kv_key_alloc(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_export(uint32_t shard, uint8_t *out, uint32_t *len);
/* W153: the committed formation membership on a group's anchor shard. */
int efs_kv_key_membership(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_inode(uint32_t shard, efs_ino_t ino, uint8_t *out, uint32_t *len);
int efs_kv_key_pver(uint32_t shard, efs_ino_t ino, uint8_t *out, uint32_t *len);
int efs_kv_key_dentry(uint32_t shard, efs_ino_t parent, const char *name,
                      uint8_t *out, uint32_t *len);
int efs_kv_key_dentry_prefix(uint32_t shard, efs_ino_t parent,
                             uint8_t *out, uint32_t *len);
int efs_kv_key_chunk(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                     uint32_t chunk_index, uint8_t *out, uint32_t *len);
int efs_kv_key_lane(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                    uint8_t *out, uint32_t *len);
int efs_kv_key_opid(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    uint32_t epoch, uint8_t *out, uint32_t *len);
/* Salted MKDIR placement: root children scatter; nested children retain the
 * parent metadata group. Recorded legacy FileIDs keep their original shard. */
uint32_t efs_kv_mkdir_shard(efs_ino_t parent, const char *name, uint64_t salt);
uint32_t efs_kv_mkdir_scattered_shard(efs_ino_t parent, const char *name,
                                    uint64_t salt);
int efs_kv_key_ver(const uint8_t *orig, uint32_t olen, uint8_t *out, uint32_t *len);
int efs_kv_key_intent(const uint8_t *orig, uint32_t olen, uint8_t *out, uint32_t *len);
int efs_kv_key_guard(const uint8_t *orig, uint32_t olen, const uint8_t txid[16],
                     uint8_t *out, uint32_t *len);
int efs_kv_key_guard_prefix(const uint8_t *orig, uint32_t olen, uint8_t *out,
                            uint32_t *len);
int efs_kv_key_reduce(const uint8_t *orig, uint32_t olen, const uint8_t txid[16],
                      uint8_t *out, uint32_t *len);
int efs_kv_key_reduce_prefix(const uint8_t *orig, uint32_t olen, uint8_t *out,
                             uint32_t *len);
int efs_kv_key_decision(uint32_t shard, const uint8_t txid[16], uint8_t *out,
                        uint32_t *len);
int efs_kv_key_decision_retired(uint32_t shard, const uint8_t txid[16],
                                uint8_t *out, uint32_t *len);
int efs_kv_key_decision_cursor(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_dseq(uint32_t shard, efs_ino_t dir, uint8_t lane, uint8_t *out,
                    uint32_t *len);
int efs_kv_key_session(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                       uint8_t *out, uint32_t *len);
int efs_kv_key_sess_local(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint8_t *out, uint32_t *len);
int efs_kv_key_lease(uint32_t shard, efs_ino_t ino, uint64_t gen,
                     const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
                     uint8_t *out, uint32_t *len);
int efs_kv_key_lease_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                            uint8_t *out, uint32_t *len);
int efs_kv_key_lease_shard_prefix(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_lock(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t domain,
                    uint64_t lo, uint64_t hi, uint8_t owner_kind,
                    uint64_t owner_id, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    uint32_t epoch, uint8_t *out, uint32_t *len);
int efs_kv_key_lock_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                           uint8_t domain, uint8_t *out, uint32_t *len);
int efs_kv_key_lock_file_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                                uint8_t *out, uint32_t *len);
int efs_kv_key_lock_shard_prefix(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_append_cur(uint32_t shard, efs_ino_t ino, uint64_t gen,
                          uint8_t *out, uint32_t *len);
int efs_kv_key_append_rsv(uint32_t shard, efs_ino_t ino, uint64_t gen,
                          uint64_t off, uint8_t *out, uint32_t *len);
int efs_kv_key_append_rsv_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                                 uint8_t *out, uint32_t *len);
int efs_kv_key_append_rsv_shard_prefix(uint32_t shard, uint8_t *out,
                                       uint32_t *len);
/* GC record for one dead chunk generation: same trailing fields as the
 * chunk key so a scan ordered by (ino, gen, lane, ci) results. `shard` is
 * the anchor shard of the dead chunk's lane shard. */
int efs_kv_key_gc(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                  uint32_t chunk_index, uint8_t *out, uint32_t *len);
int efs_kv_key_gc_prefix(uint32_t shard, uint8_t *out, uint32_t *len);
/* W154 degraded mark for one published object (base or span) with a
 * fragment missing: the GC key layout under its own kind, so a rebuild pass
 * scans one prefix per group. `shard` is the anchor of the chunk's lane
 * shard; `gen` is the object (chunk) generation. */
int efs_kv_key_degraded(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                        uint32_t chunk_index, uint8_t *out, uint32_t *len);
int efs_kv_key_degraded_prefix(uint32_t shard, uint8_t *out, uint32_t *len);
/* Reap marker for one dead inode: [reap anchor shard][REAP][ino:8].
 * `shard` is the anchor shard of the inode's shard. */
int efs_kv_key_reap(uint32_t shard, efs_ino_t ino, uint8_t *out,
                    uint32_t *len);
/* One blob of every extended attribute on an inode. `shard` is the inode
 * shard. The value format lives with efs_meta_apply_xattr. */
int efs_kv_key_xattr(uint32_t shard, efs_ino_t ino, uint8_t *out,
                     uint32_t *len);
int efs_kv_key_reap_prefix(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_unwrap(const uint8_t *wrap, uint32_t wlen, uint8_t *orig,
                      uint32_t *olen);

#endif
