#ifndef EFS_META_APPLY_H
#define EFS_META_APPLY_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/opid.h"
#include "efs/txn.h" /* stat resolves reduction intents through a coordinator */
#include "efs/fence_view.h"

/* Applied-state SM over the ordered KV (architecture.md §5 / §10 step 3).
 * CREATE file = one atomic batch {dentry, inode, alloc} on the dentry
 * shard (LOCAL: parent shard; SPLITTING/HASHED: hashed 64-lane shard).
 * LOOKUP is layout-aware (hashed-then-local during SPLITTING; I8).
 * A live dentry whose inode row cannot be resolved is I9 (EFS_ERR_IO). */

#define EFS_META_LANES 64
#define EFS_META_INO_BYTES   128
#define EFS_META_DENT_BYTES  20
#define EFS_META_ALLOC_BYTES 8
#define EFS_META_LANE_BYTES  64

/* History sidecars keep the established inode/lane encodings unchanged.
 * Authority 64 is the inode; 0..63 are its publication lanes. The FileID
 * generation is in the key, so inode reuse cannot inherit discarded bytes. */
#define EFS_META_FENCE_INODE 64u
#define EFS_META_FENCE_BYTES (8u + 16u * EFS_FENCE_HISTORY_MAX)
int efs_meta_get_fence_history(struct efs_kv *kv, efs_ino_t ino,
                               uint64_t generation, uint8_t authority,
                               struct efs_fence_history *out);
int efs_meta_pack_fence_history(const struct efs_fence_history *h,
                                uint8_t *out, uint32_t cap, uint32_t *len);
int efs_meta_unpack_fence_history(const uint8_t *value, uint32_t len,
                                  struct efs_fence_history *out);
/* Inode-authority writer snapshot, independent of chunk/lane existence.
 * Establish inode-group read authority before calling. This is not a lane
 * publication permit: the caller must separately validate/adopt that epoch on
 * its publication authority. The floor covers only the contiguous retained
 * history suffix; older writers must fail closed rather than revive bytes. */
struct efs_meta_writer_view {
    efs_ino_t ino;
    uint64_t generation;
    uint64_t authority_epoch;
    uint64_t oldest_complete_epoch;
    struct efs_fence_history history;
};
int efs_meta_get_writer_view_tx(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t generation, efs_txn_coord_fn coord,
                                void *ctx, struct efs_meta_writer_view *out);

/* Validate the publication lane under the same collected inode authority.
 * An absent inactive lane inherits the inode epoch on first publication.
 * A present lagging lane fails BUSY; this read never mutates/adopts a stamp.
 * Caller establishes read authority on both groups before collecting. */
int efs_meta_get_writer_chunk_view_tx(struct efs_kv *kv, efs_ino_t ino,
                                      uint64_t generation, uint32_t ci,
                                      efs_txn_coord_fn coord, void *ctx,
                                      struct efs_meta_writer_view *out);

/* Lane-local admission requires an exact FileID and geometry. No inode or
 * coordinator read is permitted here: outstanding local EXCL intents are BUSY.
 * Missing authority is NOT_FOUND, requiring cold transactional bootstrap. */
#define EFS_META_LANE_AUTHORITY_BYTES 32u
int efs_meta_get_lane_writer_view(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t generation, uint32_t ci,
                                  uint32_t chunk_size,
                                  struct efs_meta_writer_view *out);
struct efs_meta_lane_bootstrap {
    struct efs_meta_writer_view view;
    uint64_t active_lanes, base_size, lane_epoch, lane_seq;
    uint32_t chunk_size;
    uint8_t lane, lane_present;
};
#define EFS_META_LANE_BOOTSTRAP_BYTES (80u + EFS_META_FENCE_BYTES)
/* Cold capture needs inode and lane authority; chunk_size must come from
 * the configured export geometry, not an unvalidated application request.
 * Apply freezes inode/history
 * first, then atomically installs lane stamp/history/authority. Same-shard
 * participants resolve together through the existing transaction machinery. */
int efs_meta_capture_lane_bootstrap(struct efs_kv *kv, efs_ino_t ino,
                                    uint64_t generation, uint32_t ci,
                                    uint32_t chunk_size, efs_txn_coord_fn coord,
                                    void *ctx, struct efs_meta_lane_bootstrap *out);
int efs_meta_encode_lane_bootstrap(const struct efs_meta_lane_bootstrap *q,
                                   uint8_t authority, uint8_t *key, uint32_t *kl,
                                   uint8_t pay[EFS_META_LANE_BOOTSTRAP_BYTES]);
int efs_meta_apply_lane_bootstrap(struct efs_kv *kv, const struct efs_txid *t,
                                  const struct efs_txn_parts *parts,
                                  const uint8_t *key, uint32_t kl,
                                  const uint8_t *pay, uint32_t plen);

/* Atomic single-authority fence, for the future serialized TRUNCATE path.
 * Caller owns Raft apply serialization and cross-authority coordination.
 * This does not replace the current public truncate command by itself. */
int efs_meta_apply_content_fence(struct efs_kv *kv, efs_ino_t ino,
                                 uint64_t generation, uint8_t authority,
                                 uint64_t expected_epoch, uint64_t epoch,
                                 uint64_t size, uint64_t now);

/* Prepare one authority's logical shrink as an atomic stamp/history
 * EXCL pair. expected_active_lanes guards the inode's captured participant
 * set and expected_base_size guards shrink/extend classification; expected_seq
 * guards a lane against ordinary publication. Inode ignores expected_seq;
 * lanes ignore expected_active_lanes and expected_base_size. Full history is BUSY
 * before any intent is installed. Coordinate/decide/resolve through txn.h;
 * establish participant authority first and serialize on its apply log.
 * Committed-decision retries belong to the coordinator, not a fresh PREPARE. */
int efs_meta_prepare_content_fence(struct efs_kv *kv, const struct efs_txid *t,
                                   const struct efs_txn_parts *parts,
                                   efs_ino_t ino, uint64_t generation,
                                   uint8_t authority, uint64_t expected_epoch,
                                   uint64_t expected_seq,
                                   uint64_t expected_active_lanes,
                                   uint64_t expected_base_size,
                                   uint64_t epoch, uint64_t size, uint64_t now);

#define EFS_META_FENCE_PREPARE_BYTES 65u
struct efs_meta_fence_prepare {
    efs_ino_t ino;
    uint64_t generation;
    uint8_t authority;
    uint64_t expected_epoch;
    uint64_t expected_seq;
    uint64_t expected_active_lanes;
    uint64_t expected_base_size;
    uint64_t size;
    uint64_t now;
};
/* PREPARE subtype, shared by server/simulator; epoch is expected_epoch+1.
 * Encodes the authority's stamp key plus a fixed big-endian payload. */
int efs_meta_encode_fence_prepare(const struct efs_meta_fence_prepare *request,
                                  uint8_t *key, uint32_t *klen,
                                  uint8_t payload[EFS_META_FENCE_PREPARE_BYTES]);
int efs_meta_apply_fence_prepare(struct efs_kv *kv, const struct efs_txid *t,
                                 const struct efs_txn_parts *parts,
                                 const uint8_t *key, uint32_t klen,
                                 const uint8_t *payload, uint32_t plen);

#define EFS_META_RESIZE_SHRINK 1u
#define EFS_META_RESIZE_EXTEND 2u
#define EFS_META_RESIZE_PREPARE_BYTES 74u
struct efs_meta_resize_prepare {
    struct efs_meta_fence_prepare fence;
    uint64_t epoch; /* inode's global target; separate from lane's observed epoch */
    uint8_t action;
};
int efs_meta_encode_resize_prepare(const struct efs_meta_resize_prepare *q,
                                   uint8_t *key, uint32_t *klen,
                                   uint8_t pay[EFS_META_RESIZE_PREPARE_BYTES]);
int efs_meta_apply_resize_prepare(struct efs_kv *kv, const struct efs_txid *t,
                                  const struct efs_txn_parts *parts,
                                  const uint8_t *key, uint32_t klen,
                                  const uint8_t *pay, uint32_t plen);

struct efs_meta_resize_plan {
    struct efs_txn_parts parts;
    uint32_t count;
    struct efs_meta_resize_prepare requests[EFS_META_LANES + 1];
};
/* Handler-only capture under established authority on every relevant shard.
 * Committed intents are visible; double collect and decision validation bound
 * retries. PREPARE revalidates the bitmap/base size and every lane sequence.
 * Open append reservations return BUSY. Output stays unchanged on failure. */
int efs_meta_capture_resize(struct efs_kv *kv, efs_ino_t ino, uint64_t size,
                             uint64_t now, efs_txn_coord_fn coord, void *ctx,
                             struct efs_meta_resize_plan *out);
struct efs_meta_resize_ops {
    int (*prepare)(void *, const struct efs_txid *, const struct efs_txn_parts *,
                     const struct efs_meta_resize_prepare *);
    int (*decide)(void *, const struct efs_txid *, const struct efs_txn_parts *, int);
    int (*resolve)(void *, const struct efs_txid *, const struct efs_txn_parts *, int);
};
/* Fresh txid per capture/attempt. Freeze inode first, then active lanes;
 * durable COMMIT precedes resolution. A failed/ambiguous PREPARE requires
 * durable ABORT before resolution. An ambiguous COMMIT is never aborted or
 * dropped here: retain the intents for the existing transaction recovery. */
int efs_meta_execute_resize(const struct efs_meta_resize_plan *plan,
                             const struct efs_txid *t,
                             const struct efs_meta_resize_ops *ops, void *ctx);

#define EFS_META_PROFILE_K2F1 1u

#define EFS_META_LAYOUT_LOCAL     0
#define EFS_META_LAYOUT_SPLITTING 1
#define EFS_META_LAYOUT_HASHED    2
#define EFS_META_DENT_TOMBSTONE   0xFFFFFFFFu

/* Times are nanoseconds since the epoch. The `base_*` naming is load-bearing:
 * for a file being written, the authoritative mtime/ctime is the MAX of these
 * and the active lanes' stamps (§7.3), because routing every writer's
 * timestamp to the inode row would rebuild the per-file serialization point
 * the lanes exist to remove. base_size is the same idea, and is also what
 * survives a truncate once the lanes are epoch-fenced.
 *
 * mtime_gen guards only mtime, and only `utimens` bumps it — that is the one
 * operation that can move a timestamp BACKWARDS, so it is the only one that
 * needs to invalidate older lane stamps. Every other time source is a
 * monotone "now" where plain MAX is already correct, and guarding ctime would
 * create the backwards ctime it was meant to prevent. */
struct efs_meta_row {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t mode;
    uint32_t nlink;
    efs_ino_t parent;
    uint64_t base_size;
    uint64_t active_lanes;
    uint64_t content_epoch;
    uint8_t layout;
    uint32_t nents; /* LOCAL immediate children; frozen after spread */
    uint64_t layout_epoch;
    uint64_t used_shards;
    uint32_t uid;
    uint32_t gid;
    uint64_t base_mtime;
    uint64_t base_atime;
    uint64_t base_ctime;
    uint64_t mtime_gen;
    uint64_t parent_version;
};

struct efs_meta_dentry {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t type;
};

/* Leader-stamped inputs to a mutation.
 *
 * Apply must be a pure function of the Raft entry, so a timestamp or an
 * identity has to arrive IN the entry rather than be read at apply time: if
 * apply called clock_gettime() or looked at the process credentials, two
 * replicas applying the same committed entry would compute different state
 * and the KV replicas would silently diverge. The leader reads the clock
 * once, puts it in the entry, and every replica applies that same value.
 *
 * `now` is nanoseconds since the epoch. Implicit time updates MAX-clamp
 * against the stored value, because CLOCK_REALTIME can step backwards and a
 * file's mtime must not. */
struct efs_meta_attrs {
    uint32_t uid;
    uint32_t gid;
    uint64_t now;
};

struct efs_meta_chunk {
    uint64_t generation;
    uint32_t coding_profile_id;
    uint64_t content_epoch;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

/* CAS publication: expected_gen 0 means the slot is empty. candidate_gen
 * is a unique identity, never G+1. Chunk CAS and lane MAX are one batch.
 *
 * `now` is the leader-stamped time the lane's mtime/ctime marks take, for
 * the same determinism reason struct efs_meta_attrs exists. */
struct efs_meta_pub {
    efs_ino_t ino;
    uint32_t chunk_index;
    uint64_t new_size;
    uint64_t now;
    uint64_t expected_gen;
    uint64_t candidate_gen;
    uint64_t content_epoch;
    uint32_t coding_profile_id;
    struct efs_meta_chunk ch;
    /* Lane-local mode: the entry is applied on the LANE's group, whose KV
     * does not hold the inode row (the row lives on the inode group). The
     * proposing host read the row under ReadIndex on the inode group and
     * carries the FileID fields here; the apply skips the inode row
     * entirely (no active_lanes/base_size touch — those are the inode
     * group's keys) and takes the truncate fence from the lane's own
     * fenced_epoch. inode_gen must be the row's generation (0 is INVAL);
     * mtime_gen is the row's mtime_gen for the lane mtime guard. */
    uint64_t inode_gen;
    uint64_t mtime_gen;
    uint8_t lane_local;
    /* delta_len > 0 appends an immutable span instead of CAS-replacing
     * the chunk image. expected_gen is still the base image's generation
     * (0 if the chunk has no base object yet). A full CAS (delta_len == 0)
     * commits only when the live delta list is exactly (delta_base_n,
     * delta_base_seq) — the list the writer's merged image includes. */
    uint32_t delta_off;
    uint32_t delta_len;
    uint32_t delta_base_n;
    uint64_t delta_base_seq;
};

/* One delta stored in the chunk value's trailer. `seq` is the lane
 * sequence assigned at apply, so the list order is the Raft order. */
struct efs_meta_delta {
    uint32_t off;
    uint32_t len;
    uint64_t generation;
    uint64_t content_epoch;
    uint64_t seq;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

struct efs_meta_chunk_view {
    struct efs_meta_chunk base;
    struct efs_meta_delta deltas[EFS_FENCE_PART_MAX - 1];
    uint32_t ndelta;
    uint64_t delta_seq;
    uint64_t fence_epoch; /* exact lane stamp required by sweep/publication CAS */
    struct efs_fence_view bytes; /* captured masks, independent of history lifetime */
};
/* Caller establishes authority on the lane's Raft group first. This bounded
 * double collect rejects a row/history/stamp mixture from different applies.
 * It does not pin fragments; an object lost to GC requires fresh-view retry. */
int efs_meta_get_chunk_view(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint32_t chunk_index, uint32_t chunk_size,
                            struct efs_meta_chunk_view *out);
/* Handler variant: committed EXCL stamp/history replacements are visible
 * before RESOLVE. Undecided decisions are validated across the whole collect.
 * NULL coordinator retains the plain apply-side behavior. */
int efs_meta_get_chunk_view_tx(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint32_t chunk_index, uint32_t chunk_size,
                               efs_txn_coord_fn coord, void *ctx,
                               struct efs_meta_chunk_view *out);
/* Single-row bounded sweep CAS. replacement is a fully materialized/PUT
 * image from source; NULL requests DEL and is legal only if all parts are
 * fenced out. Exact row identities/masks and lane epoch are revalidated.
 * No fragment I/O occurs in apply. Caller serializes on the lane's log. */
int efs_meta_apply_sweep_chunk(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint32_t chunk_index,
                               const struct efs_meta_chunk_view *source,
                               const struct efs_meta_chunk *replacement);

/* Creates the root inode if absent. `now` is the leader-stamped time it is
 * born with, for the same reason every other mutation takes one.
 * `init` is mkfs with salt 0. Salt is the per-export value MKDIR scatter
 * hashes with (architecture.md §7.4). */
int efs_meta_apply_init(struct efs_kv *kv, uint64_t now);
int efs_meta_apply_mkfs(struct efs_kv *kv, uint64_t now, uint64_t salt);
/* Salt read tries the ROOT-shard record first, then the even group's
 * anchor-shard record (EFS_MD_CMD_SALT) — a node hosting only the even
 * group never applied MKFS. EFS_ERR_NOT_FOUND when neither anchor carries
 * a record (never silently 0: placement is derived from this value). */
int efs_meta_apply_export_salt(struct efs_kv *kv, uint64_t *out);
int efs_meta_apply_salt_record(struct efs_kv *kv, uint32_t anchor,
                               uint64_t salt);
int efs_meta_apply_get_inode(struct efs_kv *kv, efs_ino_t ino,
                             struct efs_meta_row *out);
/* Handler-side row read: a committed EXCL replacement is served even when
 * the old row exists (visibility at the decision, I17).
 * NULL coord = plain read. Never from the apply. */
int efs_meta_apply_get_inode_tx(struct efs_kv *kv, efs_ino_t ino,
                                efs_txn_coord_fn coord, void *ctx,
                                struct efs_meta_row *out);
/* Handler-side source snapshot for rename/unlink/rmdir. PREPARE must compare
 * each captured value (including absence), not only txn version sidecars. */
struct efs_meta_dentry_drop {
    uint32_t psh, hsh;
    uint8_t k_loc[EFS_KV_KEY_MAX];
    uint8_t k_hash[EFS_KV_KEY_MAX];
    uint8_t v_tomb[EFS_META_DENT_BYTES];
    uint32_t kl, kh;
    uint8_t loc_value[EFS_META_DENT_BYTES], hash_value[EFS_META_DENT_BYTES];
    uint32_t loc_len, hash_len;
    int del_loc, put_tomb, del_hash;
};

int efs_meta_capture_dentry_drop(struct efs_kv *kv,
                                  const struct efs_meta_row *parent_row,
                                  efs_ino_t parent, const char *name,
                                  const struct efs_meta_dentry *expected,
                                  struct efs_meta_dentry_drop *out);

int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out);
/* Handler-side lookup: a COMMITted EXCL intent is the dentry. NULL coord
 * = plain lookup. Never from the apply. */
int efs_meta_apply_lookup_tx(struct efs_kv *kv, efs_ino_t parent,
                             const char *name, efs_txn_coord_fn coord,
                             void *ctx, struct efs_meta_dentry *out);
/* LOOKUP + inode fetch. Dentry miss = NOT_FOUND; inode miss = I9 (IO). */
int efs_meta_apply_resolve(struct efs_kv *kv, efs_ino_t parent, const char *name,
                           struct efs_meta_dentry *dent, struct efs_meta_row *row);
int efs_meta_apply_resolve_tx(struct efs_kv *kv, efs_ino_t parent,
                              const char *name, efs_txn_coord_fn coord,
                              void *ctx, struct efs_meta_dentry *dent,
                              struct efs_meta_row *row);

/* One hop of a LOOKUP_PATH walk: the inode the component resolved to, with
 * the fields the caller needs for the ancestor exec check. Size/mtime are
 * NOT here — those are a GETATTR. */
struct efs_meta_path_hop {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};

/* Same bound as the wire RPC: a path deeper than this is ceil(depth/64)
 * calls, each resuming from the previous terminal. */
#define EFS_META_PATH_MAX 64

/* Batched ancestor resolution. `path` is slash-separated and relative to
 * `start` (0 = root); a leading slash is ignored, empty components from
 * repeated slashes are skipped. At most `cap` components are resolved in
 * this call (cap is also bounded by EFS_META_PATH_MAX). hops[0..n-2] are
 * the intermediate directories; hops[n-1] is this batch's terminal. An
 * empty path returns `start` alone.
 *
 * This is a read: it does not check search permission (credentials are not
 * in the KV) and it is not a snapshot — a concurrent unlink may make a
 * later hop fail. Intermediate not-a-directory is INVAL. A live dentry
 * whose inode row cannot be resolved is I9 (IO), never a silent miss. */
int efs_meta_apply_lookup_path(struct efs_kv *kv, efs_ino_t start,
                               const char *path, struct efs_meta_path_hop *hops,
                               uint32_t cap, uint32_t *n);
int efs_meta_apply_create_file(struct efs_kv *kv, const struct efs_meta_attrs *at,
                               efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out);
int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  const struct efs_meta_attrs *at,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out);
/* Raft apply of a committed CREATE. Always persist the child: a dual-host
 * LSM can see the parent row from another group, and a follower can miss
 * the parent row, but the log already decided the name. */
int efs_meta_apply_create_file_log(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                   efs_ino_t parent, uint32_t mode,
                                   const char *name, efs_ino_t *out);
/* Same, but the leader already chose `ino` and the parent's layout. A
 * follower that cannot see the parent row must write those keys — guessing
 * HASHED vs LOCAL from a missing parent is how dual-host and g2-only
 * replicas diverged and APPEND then saw NOT_FOUND on one voter. */
int efs_meta_apply_create_file_log_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      uint8_t layout, efs_ino_t *out);
/* Log apply with an op-id (I16): a replay of q returns the recorded
 * verdict and ino without touching the namespace; a first apply records
 * (OK, ino) in the window on the dentry shard in the same batch. ino 0 =
 * allocate here (layout ignored). q NULL or zero identity = unprotected. */
int efs_meta_apply_create_file_log_op(struct efs_kv *kv,
                                      const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      int layout, const struct efs_opid_req *q,
                                      efs_ino_t *out);
int efs_meta_apply_mkdir_log_op(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                efs_ino_t parent, uint32_t mode, const char *name,
                                efs_ino_t ino, int layout,
                                const struct efs_opid_req *q, efs_ino_t *out);
/* UNLINK / RMDIR with an op-id: replay answers the recorded OK (reply
 * ino = the removed inode; extra = nlink after, unlink only). */
int efs_meta_apply_unlink_op(struct efs_kv *kv, efs_ino_t parent, const char *name,
                             uint64_t now, const struct efs_opid_req *q);
int efs_meta_apply_rmdir_op(struct efs_kv *kv, efs_ino_t parent, const char *name,
                            uint64_t now, const struct efs_opid_req *q);
/* Leader-side pre-check: 1 = id already completed in the window on
 * `shard` (out filled), 0 = new, negative = KV error. Zero identity = 0. */
int efs_meta_apply_opid_probe(struct efs_kv *kv, uint32_t shard,
                              const struct efs_opid *id,
                              struct efs_opid_reply *out);
/* MKDIR. Child inode is scattered (mkdir_shard); dentry follows the
 * parent's layout. One atomic batch — the host only proposes this when
 * parent, dentry, and child shards share a Raft group (same log). Cross-
 * group mkdir stays a txn. Parent nlink++ (the child's `..`). */
int efs_meta_apply_mkdir(struct efs_kv *kv, const struct efs_meta_attrs *at,
                         efs_ino_t parent, uint32_t mode, const char *name,
                         efs_ino_t *out);
/* Same as mkdir, but the leader already chose `ino` and the parent's
 * layout. A follower that cannot see the parent must write those keys —
 * the same dual-host / g2-only split that broke file CREATE. */
int efs_meta_apply_mkdir_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                            efs_ino_t parent, uint32_t mode, const char *name,
                            efs_ino_t ino, uint8_t layout, efs_ino_t *out);
/* `now` is the leader-stamped directory mtime/ctime. LOCAL: parent row.
 * HASHED/SPLITTING: the dentry shard's dir lane (§7.4). */
int efs_meta_apply_unlink(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          uint64_t now);
/* LINK: new dentry on the dest parent shard, nlink++ on the inode shard.
 * Hardlink of a directory is INVAL. Dest name must be absent. */
int efs_meta_apply_link(struct efs_kv *kv, efs_ino_t src_parent, const char *src_name,
                        efs_ino_t dst_parent, const char *dst_name, uint64_t now);
/* File or directory rename, no replace. Directory rename walks the
 * destination ancestry and bumps `parent_version` (cycle prevention).
 * Same-name is a no-op. */
int efs_meta_apply_rename(struct efs_kv *kv, efs_ino_t src_parent,
                          const char *src_name, efs_ino_t dst_parent,
                          const char *dst_name, uint64_t now);
/* RMDIR: dest must be a directory, empty, and not root. HASHED emptiness
 * is a scan of used dir lanes; SPLITTING returns BUSY. */
int efs_meta_apply_rmdir(struct efs_kv *kv, efs_ino_t parent, const char *name,
                         uint64_t now);

/* SETATTR, mode/owner class only (chmod, chown, chgrp).
 *
 * This is the single-shard class: the inode row alone, one Raft entry
 * (architecture.md §6). The other two SETATTR classes are NOT this call —
 * `utimens` can set mtime backwards and so needs the bounded inode fence
 * over the active lanes plus an `mtime_gen` bump, and SETATTR(size) is
 * truncate. Routing either through here would leave lane stamps that are
 * newer than the value just set.
 *
 * ctime, never mtime: POSIX says a metadata change touches ctime and leaves
 * mtime alone, and mtime for a file with active write lanes does not even
 * live in this row.
 *
 * Only the 07777 mode bits are taken from `mode`; the file type is part of
 * the inode's identity and cannot be reassigned. Clearing setuid/setgid on
 * chown is a policy decision left to the caller, which can set MODE in the
 * same call — this layer applies exactly what the entry asks for.
 *
 * `expect_gen` is the generation the caller's inode handle was resolved at;
 * a mismatch is a stale handle (EFS_ERR_STALE), never a silent no-op that
 * would apply someone's chmod to a different file that reused the ino.
 * Pass 0 to skip the check for a caller that read the row itself. */
#define EFS_META_SET_MODE 0x1u
#define EFS_META_SET_UID  0x2u
#define EFS_META_SET_GID  0x4u

struct efs_meta_setattr {
    uint32_t mask;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t expect_gen;
};

int efs_meta_apply_setattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_setattr *sa);

/* One blob of extended attributes on the inode shard (EFS_KV_KIND_XATTR).
 * op is EFS_XATTR_SET or EFS_XATTR_REMOVE. flags is XATTR_CREATE/REPLACE.
 * The apply folds the op into the blob currently stored, so a later name
 * does not wipe an earlier one. An empty blob deletes the key. */
int efs_meta_apply_xattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                         uint8_t op, uint32_t flags, const uint8_t *name,
                         uint16_t nlen, const uint8_t *val, uint32_t vlen);
/* *vlen is the buffer size in and the value size out. A missing name is
 * EFS_ERR_NODATA; a missing inode is EFS_ERR_NOT_FOUND. */
int efs_meta_xattr_get(struct efs_kv *kv, efs_ino_t ino, const uint8_t *name,
                       uint16_t nlen, uint8_t *val, uint32_t *vlen);
/* Linux listxattr: names separated by NUL. *len is the buffer size in and
 * the byte count out. buf NULL reports the size only. */
int efs_meta_xattr_list(struct efs_kv *kv, efs_ino_t ino, uint8_t *buf,
                        uint32_t *len);

/* SETATTR utimens class: the bounded inode fence (§7.3 / §7.4).
 *
 * Only utimens can set a time backwards, so it is the only op that bumps
 * `mtime_gen` and pushes that generation onto every active write lane (a
 * file) or every used dir lane (a HASHED/SPLITTING directory). getattr then
 * ignores lane mtimes stamped under an older generation, which is how a
 * backwards mtime sticks instead of losing the MAX to a stale lane.
 *
 * `mtime_gen` in the entry is the absolute value the row takes, not an
 * increment — the leader reads the row, writes `row.mtime_gen + 1` into the
 * command, and apply is a pure assignment. Re-applying the same committed
 * entry is then a no-op on the generation (and a no-op on already-fenced
 * lanes). Pass 0 only when MTIME is not in the mask (atime-only does not
 * fence, because atime lives only on the row).
 *
 * mtime and atime are assigned exactly, not MAX-clamped. ctime still
 * MAX-clamps `now`, same as every other implicit time. A mask of only
 * ATIME does not bump `mtime_gen`. `expect_gen` is the stale-handle check,
 * same as mode/owner setattr. */
#define EFS_META_SET_MTIME 0x10u
#define EFS_META_SET_ATIME 0x20u

struct efs_meta_utimens {
    uint32_t mask;
    uint64_t mtime;
    uint64_t atime;
    uint64_t expect_gen;
    uint64_t mtime_gen;
    /* Nonzero: this apply is the other group's lanes only (the inode
     * row is not in this KV). */
    uint64_t lane_bits;
};

int efs_meta_apply_utimens(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_utimens *u);

/* What stat() returns. `lanes` and `attempts` are observability: `attempts`
 * above 1 means the collect had to retry against concurrent writers. */
struct efs_meta_stat {
    efs_ino_t ino;
    uint64_t generation;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t mtime;
    uint64_t atime;
    uint64_t ctime;
    /* D17: present chunk rows, summed over the file's lanes (0 for dirs).
     * The client's st_blocks source for files it never wrote. */
    uint64_t alloc;
    uint32_t lanes;
    uint32_t attempts;
};

/* Bounded, because a continuously-written file could otherwise starve the
 * retry loop forever. */
#define EFS_META_STAT_TRIES 4

/* GETATTR: the validated double collect (§7.3).
 *
 * Size and the write timestamps do not live on the inode row — they are
 * high-water marks spread over the file's active write lanes, so that
 * writers never serialize on the inode's leader. A HASHED directory is
 * the same shape: POSIX mtime/ctime of the directory live on per-dentry
 * shard dir lanes (`used_shards`), reduced the same way. Reducing with
 * MAX is commutative, but a MAX over several independently-linearizable
 * shards is NOT automatically an atomic snapshot: with two lanes at 100,
 * a reader that takes A=100, then sees writers commit A=1000 and B=500,
 * then takes B=500, returns 500 — a size (or a time) the object never had.
 *
 * So the lane vector has to be shown to have existed simultaneously: read
 * the inode row, collect the active lanes, collect their sequence numbers
 * again, re-check the transactions that were undecided, and re-read the
 * inode row. If nothing moved, every value held between the end of the first
 * collect and the start of the second, so their MAX is linearizable. If
 * anything moved, retry. The common path is 2·|lanes|+2 point reads, and
 * |lanes| is 1 for an ordinary file.
 *
 * `coord` resolves a transaction's decision; it is how the collect
 * distinguishes a committed reduction from a merely prepared one, and an
 * unreachable authority is a resource error rather than "absent" (I9).
 *
 * Returns EFS_ERR_BUSY when the retries are exhausted. The spec's escape
 * hatch for that case is a read-only multi-shard transaction over the lane
 * set, which is NOT built yet — BUSY is safe but less live than the design
 * calls for, and the fallback belongs with the rest of the read-only
 * transaction work. */
int efs_meta_apply_getattr(struct efs_kv *kv, efs_ino_t ino,
                           efs_txn_coord_fn coord, void *ctx,
                           struct efs_meta_stat *out);

/* Resumable readdir cursor. `src` is which source is being scanned (a dir
 * lane, or the pre-split local shard) and `name` is the last name already
 * returned from it, so a resume seeks instead of rescanning: a readdir that
 * restarts at the front of the directory each page is O(entries) per page.
 *
 * Zero it to start. It stays valid across a leader change because it
 * describes a position in the KV, not in any leader's memory. */
struct efs_meta_dir_cursor {
    uint32_t src;
    uint8_t done;
    char name[EFS_MAX_NAME];
};

struct efs_meta_dir_ent {
    char name[EFS_MAX_NAME];
    struct efs_meta_dentry d;
};

/* Fills up to max entries and advances cur. Only cur->done means the end;
 * a short page does not, so loop until done rather than until a partial
 * result.
 *
 * The guarantee is deliberately weak (architecture.md §3): every name
 * returned existed at some point during the scan and no name is returned
 * twice, but this is NOT a snapshot — a concurrent create or unlink may or
 * may not be observed. */
int efs_meta_apply_readdir(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n);
/* Last-link reclaim: nlink==0 AND no open leases (I19, L6). Else BUSY.
 * Deletes the inode row and writes the reap marker (EFS_KV_KIND_REAP on the
 * inode group's anchor shard) in the same batch — the marker is what the
 * background reaper needs to sweep the dead file's lanes and GC its
 * fragments (L7), and it must be atomic with the row delete so a crash can
 * never strand the chunks. */
int efs_meta_apply_reclaim(struct efs_kv *kv, efs_ino_t ino);
int efs_meta_apply_publish(struct efs_kv *kv, const struct efs_meta_pub *p);
/* Which rule STALEd the last efs_meta_apply_publish on this thread. */
enum {
    EFS_PUB_STALE_NONE = 0,
    EFS_PUB_STALE_ROW_EPOCH = 1,  /* content_epoch < row's (truncate) */
    EFS_PUB_STALE_BASE_CAS = 2,   /* full image: expected_gen != committed */
    EFS_PUB_STALE_LANE_EPOCH = 3, /* content_epoch < lane fenced_epoch */
    EFS_PUB_STALE_OVERLAP = 4,    /* span overlaps a live span */
    EFS_PUB_STALE_CHAIN_FULL = 5, /* EFS_CHUNK_DELTA_MAX live spans */
    EFS_PUB_STALE_FOLD_LIST = 6,  /* fold: delta_base_n/seq moved */
};
int efs_meta_apply_publish_stale_why(void);
int efs_meta_apply_epoch_fence(struct efs_kv *kv, efs_ino_t ino);
/* SETATTR(size): content_epoch bump, base_size, per-lane epoch fence +
 * range-delete of chunk-map entries beyond the new size, optional tail
 * chunk CAS (§7.3). `expect_gen` is the stale-handle check (0 = skip).
 * `tail` is NULL when the new size is chunk-aligned or zero. */
struct efs_meta_truncate {
    uint64_t expect_gen;
    uint64_t size;
    const struct efs_meta_pub *tail;
    /* Which active lanes THIS entry fences + range-deletes. The truncate
     * entry runs on the inode group, whose KV holds only the lanes whose
     * shard maps to that group; lanes on other groups are fenced by
     * separate EFS_MD_CMD_LANE_FENCE entries proposed to their own groups
     * BEFORE this entry (a group's log only writes its own shards' keys).
     * ~0ULL = all lanes (single-group deployments, the simulator). */
    uint64_t lane_mask;
    /* The tail chunk's lane lives on another group: the tail CAS is NOT in
     * this entry (it is lane-local-published after commit, fenced by the
     * LANE_FENCE that already landed on that lane). tail must be NULL. */
    uint8_t tail_external;
    /* Unaligned size and no tail image: keep the existing tail chunk
     * (a generation-0 span row). Do not publish a zero stub over it. */
    uint8_t keep_tail;
};

int efs_meta_apply_truncate(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                            const struct efs_meta_truncate *t);
/* EFS_MD_CMD_ACTIVATE_LANE: set one active_lanes bit on the inode row.
 * Idempotent and monotonic; NOT_FOUND (deleted inode) is a no-op OK (P3). */
int efs_meta_apply_activate_lane(struct efs_kv *kv, efs_ino_t ino,
                                 uint8_t lane);
/* Same, OR a mask of lanes in one inode PUT. */
int efs_meta_apply_activate_lanes(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t mask);
/* EFS_MD_CMD_LANE_FENCE: one lane's share of a truncate, applied on the
 * LANE's group: fenced_epoch = new_epoch, max_end = 0, seq++, then the
 * range delete of that lane's chunk entries beyond size (tail_ci kept),
 * subtracting what it dropped from the lane's present count (D17).
 * Idempotent via the epoch guard (a replay with the same new_epoch after
 * the lane already fenced at >= new_epoch is a no-op OK). */
int efs_meta_apply_lane_fence(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane, uint64_t new_epoch, uint64_t size,
                              uint32_t tail_ci, uint8_t has_tail);

/* Data-plane GC (spec L7). A GC record (EFS_KV_KIND_GC on the lane group's
 * anchor shard) is written in the SAME batch as every chunk-key delete —
 * truncate range delete, publish CAS supersede, and lane sweep — so a dead
 * generation's fragment set (nodes + per-fragment checksums) survives the
 * chunk key it came from. The checksums are the conditional-delete token:
 * fragment paths do not carry the generation, so the executor deletes only
 * when the on-disk .sum sidecar matches the dead generation (a mismatch
 * means a newer generation occupies the slot; the dead bytes are already
 * gone and the record may be acked). */
#define EFS_META_GC_VAL (4 * EFS_NUM_FRAGMENTS + 4 + \
                         EFS_HASH_SIZE * EFS_NUM_FRAGMENTS) /* 112 */
/* Reap marker value: [inode_generation:8][active_lanes:8]. */
#define EFS_META_REAP_VAL 16

void efs_meta_pack_gc(uint8_t *p, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      uint8_t ack_bits,
                      const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);
int efs_meta_unpack_gc(const uint8_t *p, uint32_t n,
                       efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                       uint8_t *ack_bits,
                       uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);
void efs_meta_pack_reap(uint8_t *p, uint64_t generation,
                        uint64_t active_lanes);
int efs_meta_unpack_reap(const uint8_t *p, uint32_t n, uint64_t *generation,
                         uint64_t *active_lanes);

/* EFS_MD_CMD_LANE_SWEEP on the lane's group: delete every chunk key of
 * (ino, gen, lane), emitting a GC record per chunk on this group's anchor
 * shard, then delete the lane key. Idempotent: a swept lane scans empty.
 * Loops in bounded batches internally, so one entry drains a whole lane. */
int efs_meta_apply_lane_sweep(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane);
/* EFS_MD_CMD_REAP_DONE on the inode's group after every active lane was
 * swept: delete leftover append cursor/reservation records, then the reap
 * marker. Defensively deletes the inode row too if it somehow still exists
 * with nlink==0 and no leases; a live row (re-linked) just loses the
 * marker. Idempotent: a missing marker is a no-op OK. */
int efs_meta_apply_reap_done(struct efs_kv *kv, efs_ino_t ino, uint64_t gen);

struct efs_gc_ack_item {
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci;
    uint8_t lane;
    uint8_t frag; /* index into the record's nodes[]/checksums[] */
};

/* EFS_MD_CMD_GC_ACK: fragment `frag` of each item was deleted (or was
 * already gone) on its target node. Sets the ack bit; when all
 * EFS_NUM_FRAGMENTS bits are set the GC record is deleted. Missing records
 * are skipped (replay after retirement). At most EFS_META_GC_ACK_MAX items
 * per entry (was 16; entries written before Sep 30 2026 are all under it). */
#define EFS_META_GC_ACK_MAX 128
int efs_meta_apply_gc_ack(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                          uint32_t n);

/* D26: per-anchor pending-GC watermark. The apply bumps it where a GC
 * record is queued (gc_queue) and lowers it where one retires
 * (gc_ack_apply), so the leader's frag pass costs one peek when the
 * anchor has nothing to collect instead of a full prefix scan. The count
 * is in-memory and starts unknown (-1 from peek): the GC thread derives
 * it once from a prefix scan (recovery, snapshot import) and the apply
 * keeps it in step from then on. Deltas while unknown accumulate so the
 * derive can fold them in; a drift is only ever upward (an extra bounded
 * scan), never below the live record count. */
void efs_meta_gc_pending_note(uint32_t anchor, int delta);
int64_t efs_meta_gc_pending_peek(uint32_t anchor);
void efs_meta_gc_pending_derived(uint32_t anchor, uint64_t n);
void efs_meta_gc_pending_invalidate(uint32_t anchor);
/* Clamp an upward drift: the GC thread calls this after a pass scanned
 * the anchor's whole prefix and found nothing. Fails (returns 0) if a
 * note raced the scans, so a live record is never zeroed away. */
int efs_meta_gc_pending_zero_if(uint32_t anchor, int64_t expect);

/* O_APPEND: serialized EOF reservation on the inode shard, then ordinary
 * distributed publish. `op` is required (I16: a retried reserve must recover
 * the same offset). `coord` is the same getattr collect so pending reductions
 * count as EOF. */
#define EFS_META_APPEND_COMPLETED    1
#define EFS_META_APPEND_ABORTED_HOLE 2
#define EFS_META_APPEND_FENCED_HOLE  3

int efs_meta_apply_append_reserve(struct efs_kv *kv, efs_ino_t ino, uint64_t len,
                                  const struct efs_opid *op,
                                  efs_txn_coord_fn coord, void *ctx,
                                  uint64_t *off_out);
int efs_meta_apply_append_resolve(struct efs_kv *kv, efs_ino_t ino, uint64_t off,
                                  int outcome);
/* Hosted path has no op-id yet (zero UUID). The reserved eof rides the
 * apply-result ring extra, not a post-wait watermark read. nopen==0
 * means no burst. */
int efs_meta_apply_append_state(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t *watermark, uint64_t *frontier,
                                uint32_t *nopen);
/* 0 = this uuid may reserve (none open, or only this uuid). 1 = another
 * uuid holds an OPEN reservation. <0 = KV error. In-place EC fragments
 * cannot merge two clients' GET+PUT of the same chunk, so a second
 * appender waits until the first REPORT resolves nopen. */
int efs_meta_apply_append_foreign(struct efs_kv *kv, efs_ino_t ino,
                                  const uint8_t uuid[EFS_OPID_UUID_LEN]);
/* OPEN reservations only. *n is capacity in, count out (capped). */
int efs_meta_apply_append_open(struct efs_kv *kv, efs_ino_t ino, uint64_t *offs,
                               uint64_t *lens, uint32_t *n);
/* Resolve OPEN reservations of (uuid, epoch) on this shard as FENCED_HOLE
 * (I23 / §7.3). Called from LEASE_DROP after the revocation barrier. */
int efs_meta_apply_append_drop_session(struct efs_kv *kv, uint32_t shard,
                                       const uint8_t uuid[EFS_OPID_UUID_LEN],
                                       uint32_t epoch);
/* Resolve every OPEN reservation on ino as ABORTED_HOLE. Called on the
 * last-lease edge: the reserving writer is gone, so the range becomes a
 * committed zero hole and the append frontier can settle (§7.3). */
int efs_meta_apply_append_drain_file(struct efs_kv *kv, efs_ino_t ino);
int efs_meta_apply_get_chunk(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                             struct efs_meta_chunk *out);
/* Deltas layered on the base image. *n is the count (capped by `cap`).
 * *newest is the last delta's seq, or 0 when there are none. NOT_FOUND
 * when the chunk row itself is absent. */
/* One KV get: 1 when the row already holds this publish, 0 when it does
 * not, NOT_FOUND when the chunk row is absent. A full image
 * (span_len == 0) is held when `gen` is the base generation. A span is
 * held when a trailer entry has `gen` AND the same [off,len) (or is a
 * len-0 tombstone of it). Gen alone is not identity: the object name is
 * a content hash, and two clients that merge to the same image PUT one
 * object under two ranges (IOR hard 9x4, Sep 30: the second range was
 * "held" and never recorded). *committed (optional) gets the base
 * generation. The caller supplies the inode generation. */
int efs_meta_apply_chunk_holds(struct efs_kv *kv, efs_ino_t ino,
                               uint64_t inode_gen, uint32_t chunk_index,
                               uint64_t gen, uint32_t span_off,
                               uint32_t span_len, uint64_t *committed);
int efs_meta_apply_get_chunk_deltas(struct efs_kv *kv, efs_ino_t ino,
                                    uint32_t chunk_index,
                                    struct efs_meta_delta *out, uint32_t cap,
                                    uint32_t *n, uint64_t *newest);
int efs_meta_apply_get_chunk_row(struct efs_kv *kv, efs_ino_t ino,
                                 uint64_t inode_gen, uint32_t chunk_index,
                                 struct efs_meta_chunk *out,
                                 struct efs_meta_delta *ds, uint32_t cap,
                                 uint32_t *n, uint64_t *newest);
uint64_t efs_meta_candidate_gen(const uint8_t uuid[16], uint32_t session_epoch,
                                uint64_t seq, uint32_t chunk_index,
                                uint32_t retry);
int efs_meta_apply_check(struct efs_kv *kv);
int efs_meta_pack_inode(const struct efs_meta_row *r, uint8_t *out, uint32_t cap);
int efs_meta_unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r);
int efs_meta_pack_dentry(const struct efs_meta_dentry *d, uint8_t *out,
                         uint32_t cap);
int efs_meta_unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d);
int efs_meta_apply_peek_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next);
/* HASHED/SPLITTING dest CREATE: pack the dir-lane MAX stamp that lives on
 * the dentry shard so the parent leader is not on the create path. */
int efs_meta_stamp_dir_lane(struct efs_kv *kv, const struct efs_meta_row *dir,
                            const char *name, uint64_t now, uint8_t *key,
                            uint32_t *klen, uint8_t *val, uint32_t cap);

#endif
