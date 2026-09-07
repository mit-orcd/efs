#ifndef EFS_META_APPLY_H
#define EFS_META_APPLY_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/opid.h"
#include "efs/txn.h" /* stat resolves reduction intents through a coordinator */

/* Applied-state SM over the ordered KV (architecture.md §5 / §10 step 3).
 * CREATE file = one atomic batch {dentry, inode, alloc} on the dentry
 * shard (LOCAL: parent shard; SPLITTING/HASHED: hashed 64-lane shard).
 * LOOKUP is layout-aware (hashed-then-local during SPLITTING; I8).
 * A live dentry whose inode row cannot be resolved is I9 (EFS_ERR_IO). */

#define EFS_META_LANES 64
#define EFS_META_INO_BYTES   128
#define EFS_META_DENT_BYTES  20
#define EFS_META_ALLOC_BYTES 8

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
};

/* Creates the root inode if absent. `now` is the leader-stamped time it is
 * born with, for the same reason every other mutation takes one.
 * `init` is mkfs with salt 0. Salt is the per-export value MKDIR scatter
 * hashes with (architecture.md §7.4); a missing record reads as 0. */
int efs_meta_apply_init(struct efs_kv *kv, uint64_t now);
int efs_meta_apply_mkfs(struct efs_kv *kv, uint64_t now, uint64_t salt);
int efs_meta_apply_export_salt(struct efs_kv *kv, uint64_t *out);
int efs_meta_apply_get_inode(struct efs_kv *kv, efs_ino_t ino,
                             struct efs_meta_row *out);
int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out);
/* LOOKUP + inode fetch. Dentry miss = NOT_FOUND; inode miss = I9 (IO). */
int efs_meta_apply_resolve(struct efs_kv *kv, efs_ino_t parent, const char *name,
                           struct efs_meta_dentry *dent, struct efs_meta_row *row);

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
/* Last-link reclaim: nlink==0 AND no open leases (I19, L6). Else BUSY. */
int efs_meta_apply_reclaim(struct efs_kv *kv, efs_ino_t ino);
int efs_meta_apply_publish(struct efs_kv *kv, const struct efs_meta_pub *p);
int efs_meta_apply_epoch_fence(struct efs_kv *kv, efs_ino_t ino);
/* SETATTR(size): content_epoch bump, base_size, per-lane epoch fence +
 * range-delete of chunk-map entries beyond the new size, optional tail
 * chunk CAS (§7.3). `expect_gen` is the stale-handle check (0 = skip).
 * `tail` is NULL when the new size is chunk-aligned or zero. */
struct efs_meta_truncate {
    uint64_t expect_gen;
    uint64_t size;
    const struct efs_meta_pub *tail;
};

int efs_meta_apply_truncate(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                            const struct efs_meta_truncate *t);

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
/* Hosted path has no op-id yet (zero UUID). After wait, the handler reads
 * watermark rather than an apply-side extra. nopen==0 means no burst. */
int efs_meta_apply_append_state(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t *watermark, uint64_t *frontier,
                                uint32_t *nopen);
/* OPEN reservations only. *n is capacity in, count out (capped). */
int efs_meta_apply_append_open(struct efs_kv *kv, efs_ino_t ino, uint64_t *offs,
                               uint64_t *lens, uint32_t *n);
int efs_meta_apply_get_chunk(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                             struct efs_meta_chunk *out);
uint64_t efs_meta_candidate_gen(const uint8_t uuid[16], uint32_t session_epoch,
                                uint64_t seq, uint32_t chunk_index,
                                uint32_t retry);
int efs_meta_apply_check(struct efs_kv *kv);
int efs_meta_pack_inode(const struct efs_meta_row *r, uint8_t *out, uint32_t cap);
int efs_meta_pack_dentry(const struct efs_meta_dentry *d, uint8_t *out,
                         uint32_t cap);
int efs_meta_unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d);
int efs_meta_apply_peek_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next);

#endif
