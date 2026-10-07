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
#define EFS_KV_KIND_DECISION 11 /* coordinator decision record (txid) */
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
/* MKDIR scatter: hash(parent, name, export_salt) & 0xFFF (§7.4). */
uint32_t efs_kv_mkdir_shard(efs_ino_t parent, const char *name, uint64_t salt);
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
