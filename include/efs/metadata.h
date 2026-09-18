#ifndef EFS_METADATA_H
#define EFS_METADATA_H

/* Table implementation: src/meta/metadata.c (Phase M step 3). Persistence
 * goes through efs/kv.h once serialize is wired (step 4). */

#include "efs/common.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* Virtual per-directory stats file (FUSE-only; not a real inode). */
#define EFS_STATS_NAME ".stats"
/* Virtual per-directory search file (FUSE-only; not a real inode). */
#define EFS_FIND_NAME ".find"
/* Minimum literal (non-'*') characters in a .find query term. */
#define EFS_FIND_MIN_TERM 4

/* EFS_FEATURE_* / EFS_FEATURES_DEFAULT live in common.h (shared with the wire
 * protocol and efs-mgmt). */

struct efs_chunk_entry {
    efs_ino_t ino;
    uint32_t chunk_index;
    efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

struct efs_inode {
    efs_ino_t ino;
    efs_ino_t parent;
    uint32_t mode;
    uid_t uid;
    gid_t gid;
    uint64_t size;
    uint64_t mtime;
    uint32_t mtime_nsec; /* nanoseconds portion of mtime (for rsync etc.) */
    uint32_t ctime_nsec; /* was alignment padding; offsets/size unchanged */
    uint64_t ctime;
    uint64_t atime; /* set on create / utimens; never bumped on read */
    uint32_t nlink;
    uint32_t atime_nsec; /* was alignment padding; offsets/size unchanged */
    char name[EFS_MAX_NAME];
    /* Directory rollups (contents only; zero on files). Immediate = direct
     * children; tree = all descendants. Size sums regular-file sizes
     * (dent-based; hard links may double-count). */
    uint64_t imm_files, imm_dirs, tree_files, tree_dirs;
    uint64_t imm_bytes, tree_bytes;
    uint64_t imm_tmin, imm_tmax, tree_tmin, tree_tmax;
    /* Small-file pack (EFSM v5): pack_ino==0 means this file has its own
     * chunks. Otherwise payload lives in pack_ino's data at [off, off+len). */
    efs_ino_t pack_ino;
    uint32_t pack_off;
    uint32_t pack_len;
};

/* Live table row: same attrs as efs_inode but the 256 B name lives in the
 * per-tab name arena (EFSM v7 already packs names separately on the wire).
 * RPC replies still fill a stack efs_inode via efs_export_inode_to_rpc. */
struct efs_inode_mem {
    efs_ino_t ino;
    efs_ino_t parent;
    uint32_t mode;
    uid_t uid;
    gid_t gid;
    uint64_t size;
    uint64_t mtime;
    uint32_t mtime_nsec;
    uint64_t ctime;
    uint64_t atime;
    uint32_t nlink;
    uint32_t name_off; /* byte offset into the owning slab's name arena */
    uint16_t name_len;
    /* Owning slab, so a row pointer resolves to its arena and its slot in
     * O(1). Bounded by EFS_META_INO_PAGE_MAX slabs, which fits u16. */
    uint16_t slab_idx;
    uint64_t imm_files, imm_dirs, tree_files, tree_dirs;
    uint64_t imm_bytes, tree_bytes;
    uint64_t imm_tmin, imm_tmax, tree_tmin, tree_tmax;
    efs_ino_t pack_ino;
    uint32_t pack_off;
    uint32_t pack_len;
};

/* One slab == one serialized inode page. names is the slab's private name
 * arena: a shared append-only arena could never give bytes back, so every
 * evict/refault cycle re-appended the whole slab's names and grew RAM
 * monotonically. Owning them per slab makes eviction actually reclaim. */
struct efs_ino_slab {
    struct efs_inode_mem *rows; /* NULL = evicted */
    char *names;
    uint32_t names_used;
    uint32_t names_cap;
    uint64_t tick;
};

/* In-memory parent → child slot list (not serialized). */
struct efs_child_vec {
    uint64_t *slots;
    uint64_t count;
    uint64_t cap;
};

/* What is left of the old fully-replicated export root: the shard geometry
 * the staging table needs to route by shard. Nothing serializes, fetches or
 * commits a root since the Raft+KV engine replaced the page flush. */
struct efs_export_root {
    uint32_t shard_count;
    uint32_t shard_bits;
};

struct efs_export {
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
    uint32_t chunk_size; /* data EC unit; default EFS_DEFAULT_CHUNK_SIZE */
    uint32_t features;   /* EFS_FEATURE_* bitmask; mirrors root.features */
    uint64_t next_ino;
    struct efs_inode_mem *inodes; /* NULL when using ino_slabs */
    struct efs_ino_slab *ino_slabs;
    uint32_t ino_slab_n;
    uint32_t ino_slabs_resident;
    uint64_t ino_slab_tick;
    uint64_t inode_count;
    uint64_t inode_capacity;
    /* Names live in per-slab arenas (struct efs_ino_slab), not one table-wide
     * blob: a shared arena cannot return bytes when a slab is evicted. */
    struct efs_chunk_entry *chunks;
    uint64_t chunk_count;
    uint64_t chunk_capacity;

    /* Open-addressing indexes (key 0 = empty). Kept in sync on
     * create/unlink/rename/set_chunk so lookups stay O(1) at scale. */
    uint64_t *ino_keys;
    uint64_t *ino_vals;
    uint64_t ino_mask;
    uint64_t *name_keys;
    uint64_t *name_vals;
    uint64_t name_mask;
    uint64_t *chunk_keys;
    uint64_t *chunk_vals;
    uint64_t chunk_mask;
    /* ino → live chunk count in THIS table (key 0 = empty). Rebuilt alongside
     * chunk_idx in export_reindex_chunks; inc/dec on set_chunk/merge/
     * remove_chunk_at. Lets drop_chunks touch only the ino's chunks instead
     * of scanning the whole chunk array under the metadata lock. */
    uint64_t *icnt_keys;
    uint32_t *icnt_vals;
    uint64_t icnt_mask;

    /* parent_ino → efs_child_vec (in-memory only; rebuilt on load/merge). */
    uint64_t *child_keys;
    uint64_t *child_vals; /* index into child_vecs */
    uint64_t child_mask;
    struct efs_child_vec *child_vecs;
    uint64_t child_vec_count;
    uint64_t child_vec_cap;

    /* Size/mtime updated via *_norollup; parent dir tree stats/times need
     * efs_export_ensure_rollups before serialize or incremental rollups. */
    int rollups_stale;
    /* Net size deltas / time touches deferred from *_norollup (applied by
     * efs_export_ensure_rollups — O(pending), not a full tree recompute). */
    efs_ino_t *pending_rollup_inos;
    int64_t *pending_rollup_deltas;
    uint8_t *pending_rollup_touch;
    uint64_t pending_rollup_count;
    uint64_t pending_rollup_cap;
    /* Bumped when rows are swap-removed, so a dirty-set snapshot can tell
     * that slot indexes it captured no longer mean the same rows. */
    uint64_t layout_epoch;
    struct efs_export_root root;
    /* On-demand tables for shard > 0. shard 0 is this export;
     * shard_tabs[i] is NULL until first create/lookup in that shard. */
    struct efs_export **shard_tabs;
    uint32_t shard_tab_cap;
    uint32_t shard_id;
    uint64_t shard_tick;
    /* Cross-client O_APPEND barrier (in-memory only, never serialized):
     * outstanding reserved-but-unflushed append end, open-addressed by ino.
     * The handler refuses a second reserve (BUSY) while one is unflushed.
     * A single-slot (ino%N) table used to evict another ino's live rsv
     * (9-way POSIX lost append lines). A crashed appender's rsv expires. */
#define EFS_APPEND_RSV_SLOTS 256
    struct {
        efs_ino_t ino;
        uint64_t end;
        uint64_t ts_ms;
    } append_rsv[EFS_APPEND_RSV_SLOTS];
};

/* Rollup bookkeeping flags for the deferred *_norollup paths. */
#define EFS_ROLLUP_TOUCH      1
#define EFS_ROLLUP_CREATE     2

/* Inode row: the compact payload followed by the name inline. 512 divides the
 * 128 KiB slab exactly, and 512-126 leaves room for a full EFS_MAX_NAME. */
#define EFS_INODE_ROW_SIZE     512
#define EFS_INO_SLAB_ROWS      (EFS_META_PAGE_SIZE / EFS_INODE_ROW_SIZE)

uint32_t efs_export_shard_of(efs_ino_t ino, uint32_t shard_bits);
/* Shard that stores the chunk mapping for (ino, chunk_index). bits==0 → 0.
 * Independent of inode-row placement (shard_of(ino)). */
uint32_t efs_export_chunk_shard_of(efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t shard_bits);
/* Table that stores the chunk mapping (loads the shard on demand). */
struct efs_export *efs_export_table_for_chunk(struct efs_export *ex,
                                              efs_ino_t ino,
                                              uint32_t chunk_index);
/* Dentry shard for a spread directory. bits==0 → 0. */
uint32_t efs_export_dentry_shard_of(efs_ino_t parent, const char *name,
                                    uint32_t shard_bits);
/* Derived from rollups: imm_files + imm_dirs >= EFS_DIR_SPREAD_MIN. */
int efs_inode_dir_is_spread(const struct efs_inode *dir);
int efs_export_dir_is_spread(struct efs_export *ex, efs_ino_t dir);
/* Owner among live node ids (sorted or not). shard_count<=1 → lowest id;
 * else live[shard % nlive]. */
efs_node_id_t efs_shard_owner_of(uint32_t shard, uint32_t shard_count,
                                 const efs_node_id_t *live, uint32_t nlive);
/* Table that owns `ino` (or parent for name ops). bits==0 → `ex`. */
struct efs_export *efs_export_table_for_ino(struct efs_export *ex, efs_ino_t ino);
struct efs_export *efs_export_table(struct efs_export *ex, uint32_t shard);
/* Existing shard table only — does not allocate. shard 0 / bits=0 → ex. */
struct efs_export *efs_export_shard_tab(struct efs_export *ex, uint32_t shard);
/* Initialize an empty export. */
void efs_export_init(struct efs_export *ex, efs_export_id_t id, const char *name);

/* Free an export. */
void efs_export_free(struct efs_export *ex);

/* Find a child inode by name under a parent. Returns 0 if found. */
int efs_export_lookup(struct efs_export *ex, efs_ino_t parent,
                      const char *name, struct efs_inode *out);

/* Find an inode by inode number. Returns 0 if found. */
int efs_export_get_inode(struct efs_export *ex, efs_ino_t ino,
                         struct efs_inode *out);

/* Create a new inode with an explicit inode number (used by clients that
 * allocate from their own namespace so concurrent clients never collide).
 * Returns the inode number or 0 on error (including if the ino is taken). */
efs_ino_t efs_export_create_with_ino(struct efs_export *ex, efs_ino_t ino_num,
                                     efs_ino_t parent, uint32_t mode,
                                     uid_t uid, gid_t gid, const char *name);

/* Authoritative upsert of a complete inode record: the incoming row wins
 * outright (unlike efs_export_merge's newer-wins). Keeps the ino/name/child
 * indexes and dentry_bytes in sync; does not touch chunks or rollups —
 * callers re-add chunks and recompute rollups. Used by the client rebase
 * that preserves uncommitted dirty state across a STALE resync. */
int efs_export_upsert_inode(struct efs_export *ex, const struct efs_inode *rec);

/* Remove an inode and all of its chunk entries. */
int efs_export_unlink(struct efs_export *ex, efs_ino_t ino);

/* Remove one directory name. If it was the last hard link, also remove chunks.
 * keep_last: drop the name but keep inode+chunks (open fds still exist). */
int efs_export_unlink_name_ex(struct efs_export *ex, efs_ino_t parent,
                              const char *name, int keep_last);

/* Add a hard link (extra name) for an existing non-directory inode. */
int efs_export_link(struct efs_export *ex, efs_ino_t src_ino,
                    efs_ino_t new_parent, const char *new_name);

/* Cross-server hardlink helpers. nlink_inc/dec mutate every local row of
 * src_ino (canonical child table + any dentry copies). link_dentry writes
 * the new name on new_parent using src as the template; src.nlink is the
 * already-bumped value. */
int efs_export_nlink_inc(struct efs_export *ex, efs_ino_t src_ino,
                         struct efs_inode *out);
int efs_export_nlink_dec_ex(struct efs_export *ex, efs_ino_t src_ino,
                            struct efs_inode *out, int keep_last);
int efs_export_link_dentry(struct efs_export *ex, const struct efs_inode *src,
                           efs_ino_t new_parent, const char *new_name);

/* Set inode size. */
int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size);

/* Update size (and mtime) without parent directory rollups. Marks
 * ex->rollups_stale; call efs_export_ensure_rollups before .stats/serialize. */
int efs_export_set_size_norollup(struct efs_export *ex, efs_ino_t ino,
                                 uint64_t size);

/* Drop chunk map entries with chunk_index >= first_chunk (truncate shrink). */
void efs_export_drop_chunks_from(struct efs_export *ex, efs_ino_t ino,
                                 uint32_t first_chunk);

/* Cache eviction (client staging table): drop every staged trace of ino —
 * chunk recs across all loaded tabs, then the inode row(s). A create
 * dual-apply stages a dentry stub on the parent's tab plus the full row on
 * the ino's tab, and hardlinks add one row per link, so every loaded tab is
 * probed. No rollup/nlink bookkeeping: this is a cache drop, not an unlink
 * (staged .stats rollups are approximate by design). Everything evicted is
 * re-fetchable from the server (getattr/GETCHUNKS/LOOKUP). Caller holds the
 * table locks. */
void efs_export_forget_ino(struct efs_export *ex, efs_ino_t ino);

/* Approximate resident bytes of the staging cache: slab rows, name arenas,
 * chunk array, indexes, child vectors — main table + loaded shard tabs.
 * Client-cap accounting (EFS_CLIENT_META_MB); not a serialization number. */
uint64_t efs_export_staged_bytes(const struct efs_export *ex);

/* Reclaim index/array/slab over-capacity after mass eviction (client staging
 * cache). Frees empty tail slabs, shrinks the chunk array, rebuilds sparse
 * hash indexes. Caller holds the table locks; not a hot-path op. */
void efs_export_compact(struct efs_export *ex);

/* Set inode mode bits, preserving the file type. */
int efs_export_set_mode(struct efs_export *ex, efs_ino_t ino, uint32_t mode);

/* Set inode owner/group. Use (uid_t)-1 or (gid_t)-1 to leave unchanged. */
int efs_export_set_owner(struct efs_export *ex, efs_ino_t ino, uid_t uid, gid_t gid);

/* Set inode modification time (seconds; clears nanoseconds). */
int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime);

/* Set inode modification time with nanosecond precision. */
int efs_export_set_mtime_ns(struct efs_export *ex, efs_ino_t ino,
                            uint64_t mtime, uint32_t mtime_nsec);

/* Like efs_export_set_mtime_ns but skips parent directory time rollups. */
int efs_export_set_mtime_ns_norollup(struct efs_export *ex, efs_ino_t ino,
                                     uint64_t mtime, uint32_t mtime_nsec);

/* If rollups_stale, rebuild directory rollups and clear the flag. */
void efs_export_ensure_rollups(struct efs_export *ex);

/* Set inode access time (seconds). Not bumped on read. */
int efs_export_set_atime(struct efs_export *ex, efs_ino_t ino, uint64_t atime);

/* Rename/move an inode. If a destination inode already exists, it is replaced
   only when the source and destination are both regular files or both empty
   directories. Returns EFS_ERR_NOT_FOUND if the source does not exist. */
int efs_export_rename(struct efs_export *ex, efs_ino_t ino,
                      efs_ino_t new_parent, const char *new_name);
int efs_export_rename_at(struct efs_export *ex, efs_ino_t old_parent,
                         const char *old_name, efs_ino_t new_parent,
                         const char *new_name);

/* Add or update a chunk entry. */
int efs_export_set_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);

/* Get a chunk entry. Returns 0 if found. */
int efs_export_get_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         struct efs_chunk_entry *out);

/* Format directory rollup fields into .stats text. Returns bytes written
 * (excluding NUL), or -1 if buf is too small / not a directory. */
int efs_export_format_stats_ex(const struct efs_export *ex,
                               const struct efs_inode *dir, char *buf,
                               size_t buflen);

/* Visit each lookupable child slot under parent (skips the dir itself,
 * ino==0 holes, and rows the name index cannot resolve). */
typedef int (*efs_child_cb)(struct efs_export *ex, uint64_t slot, void *arg);
int efs_export_foreach_child(struct efs_export *ex, efs_ino_t parent,
                             efs_child_cb cb, void *arg);
/* Return 1 if the directory has no children, 0 otherwise. */
int efs_export_dir_empty(struct efs_export *ex, efs_ino_t ino);

/* Live-row name (empty string if slot is unused). */
const char *efs_export_inode_name(const struct efs_export *ex, uint64_t slot);
/* Fill an RPC/stack efs_inode including name[256] from a live slot. */
void efs_export_inode_to_rpc(const struct efs_export *ex, uint64_t slot,
                             struct efs_inode *out);
int efs_export_inode_slot(struct efs_export *ex, efs_ino_t ino, uint64_t *slot);
int efs_export_chunk_slot(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                          uint64_t *slot);
int efs_export_needs_chunk_grow(const struct efs_export *ex);
int efs_export_reserve_chunks(struct efs_export *ex, uint64_t extra);

#endif
