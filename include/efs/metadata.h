#ifndef EFS_METADATA_H
#define EFS_METADATA_H

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
    uint64_t ctime;
    uint64_t atime; /* set on create / utimens; never bumped on read */
    uint32_t nlink;
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

/* In-memory parent → child slot list (not serialized). */
struct efs_child_vec {
    uint64_t *slots;
    uint64_t count;
    uint64_t cap;
};

/* EFSR (root) wire versions. v1 = no chunk_size; v2 = chunk_size after
 * page_count; v3 = features; v4 = two-region ino/chunk page counts; v5 = wider
 * page window; v6 = per-page fragment checksums; v7 = copy-on-write page
 * placement (page_cis[] + next_ci). */
#define EFS_META_ROOT_VERSION_V1 1
#define EFS_META_ROOT_VERSION_V2 2
#define EFS_META_ROOT_VERSION_V3 3
#define EFS_META_ROOT_VERSION_V4 4
#define EFS_META_ROOT_VERSION_V5 5
#define EFS_META_ROOT_VERSION_V6 6
#define EFS_META_ROOT_VERSION_V7 7
#define EFS_META_ROOT_VERSION_V8 8
#define EFS_META_ROOT_VERSION EFS_META_ROOT_VERSION_V8

/* Tiny fully-replicated export root. Bulk inode/chunk tables live in
 * 2+1 metadata pages under EFS_META_TABLE_INO (see efs_meta_page_*).
 * page_checksums is heap-allocated: page_count * EFS_NUM_FRAGMENTS * HASH. */
struct efs_export_root {
    uint32_t version;
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
    uint64_t next_ino;
    uint64_t generation;
    uint32_t blob_len;   /* ino_blob_len + chunk_blob_len */
    uint32_t page_count; /* ino_page_count + chunk_page_count */
    uint32_t chunk_size; /* data chunk size for this export */
    uint32_t features;   /* EFS_FEATURE_* bitmask (EFSR v3+) */
    /* EFSR v4: two-region page counts. v1–v3 leave chunk_* at 0 and treat
     * the whole blob as the inode region (legacy single-space). */
    uint32_t ino_blob_len;
    uint32_t chunk_blob_len;
    uint32_t ino_page_count;
    uint32_t chunk_page_count;
    uint8_t *page_checksums;
    /* EFSR v6: inode-range shards + exclusive write lease.
     * shard = ino & ((1 << shard_bits) - 1); root stays on shard 0.
     * mkfs uses EFS_DEFAULT_SHARD_BITS (not 0). */
    uint32_t shard_count;
    uint32_t shard_bits;
    uint64_t write_lease_id;
    uint64_t write_lease_until_ms;
    /* EFSR v7: copy-on-write page placement. page_cis[i] is the chunk_index
     * (under EFS_META_TABLE_INO) holding page i's fragments; the flush writes
     * each dirty page to a fresh ci = next_ci++ and records it here, so an
     * interrupted flush never overwrites a chunk the committed root still
     * references. next_ci is the next never-used chunk_index. Heap-allocated:
     * page_count * sizeof(uint32_t). NULL for v6 and earlier (dual-slot). */
    uint32_t *page_cis;
    uint32_t next_ci;
    /* EFSR v8: extra shards (shard > 0). Each has its own pages under
     * efs_meta_shard_table_ino(id). v7 loads as extra_shard_count=0. */
    uint32_t extra_shard_count;
    uint32_t *extra_shard_ids;
    struct efs_export_root *extra_roots;
};

/* Copy-on-write placement (EFSR v7): the root carries an explicit page_cis[]
 * so a flush writes each dirty page to a fresh chunk_index and commits the
 * root atomically. Returns nonzero if this root uses CoW placement. */
static inline int efs_export_root_is_cow(const struct efs_export_root *r)
{
    return r->version >= EFS_META_ROOT_VERSION_V7 && r->page_cis != NULL;
}

struct efs_export {
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
    uint32_t chunk_size; /* data EC unit; default EFS_DEFAULT_CHUNK_SIZE */
    uint32_t features;   /* EFS_FEATURE_* bitmask; mirrors root.features */
    uint64_t next_ino;
    struct efs_inode *inodes;
    uint64_t inode_count;
    uint64_t inode_capacity;
    /* Packed v6 dentry tail: sum of (2 + namelen) over live inode rows.
     * Maintained on create/link/unlink/rename; recomputed on deserialize. */
    uint64_t dentry_bytes;
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

    /* When set, metadata.bin stores efs_export_root (EFSR); bulk tables are
     * reconstructed from 2+1 pages. The in-memory inode/chunk arrays remain
     * the working cache after rebuild. */
    int meta_fragmented;
    /* Set when root advanced but inode/chunk tables not yet rebuilt from pages.
     * Cleared by server_rebuild_export_from_pages. */
    int meta_needs_rebuild;
    /* Server-only GET_META serialize cache (never on the wire, never in
     * metadata.bin): serializing a multi-GiB table costs seconds under the
     * server lock (9M+ strnlens) and resync storms otherwise pin the server
     * at 100% CPU re-serializing the same generation. Valid while
     * (gm_gen, gm_epoch) match the current root generation + server epoch;
     * the handler hands out a memcpy under the lock (~0.1 s) instead. */
    char *gm_blob;
    size_t gm_blob_len;
    uint64_t gm_gen;
    uint64_t gm_epoch;
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
    /* Bumped when inode/chunk rows are swap-removed so incremental meta
     * serialize knows the cached blob layout is stale. */
    uint64_t layout_epoch;
    struct efs_export_root root;
    /* EFSM blob version last serialized (5 = 420 B inodes, 6 = compact +
     * variable dentries). Deserialize accepts both. */
    uint32_t efsm_version;
    /* Phase 3: on-demand tables for shard > 0. shard 0 is this export.
     * shard_tabs[i] is NULL until first create/lookup in that shard. */
    struct efs_export **shard_tabs;
    uint32_t shard_tab_cap;
    uint32_t create_rr;     /* spread-create cursor */
    uint32_t create_stride; /* keep creates on the same owner (nlive) */
    uint32_t shard_id;
    uint64_t shard_tick;
    int shard_dirty;
    /* Bumped when the chunk table changes. Flush skips the O(chunks)
     * snapshot/serialize when this matches flushed_chunk_epoch. */
    uint64_t chunk_epoch;
    uint64_t flushed_chunk_epoch;
    /* Flush page-reuse cache: the exact region bytes the last successful
     * flush of THIS table committed, tagged with the generation it committed
     * at. The flush byte-compares each page against these instead of
     * EC-encoding + blake3-hashing all of them just to discover ~97% are
     * unchanged (that hashing was ~1 GB/s on the single flush thread and grew
     * linearly with the table). Valid only while flush_blob_gen ==
     * root.generation; any adopt/rebuild moves the root and retires it.
     * flush_blob_gen == 0 means "written but the commit did not land". */
    char *flush_blob;
    uint32_t flush_blob_ino_len;
    uint32_t flush_blob_chunk_len;
    uint64_t flush_blob_gen;
    /* Per-page dirty bits against flush_blob. flush_full=1 means every page
     * is dirty (unlink swap, adopt, or a packing shuffle). Same-count rename
     * marks the compact slot + all dentry pages so ecopy temp→final can stay
     * incremental. A missed mark would commit stale CoW pages, so unlink
     * and layout shuffles set flush_full rather than guessing. Create /
     * set_chunk / setattr mark individual pages. */
#define EFS_FLUSH_DIRTY_BYTES ((EFS_META_MAX_PAGES + 7) / 8)
    uint8_t flush_page_dirty[EFS_FLUSH_DIRTY_BYTES];
    int flush_full;
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

#define EFS_SHARD_LRU_KEEP 64

/* EFSM v5 wire sizes (fixed-width; keep in sync with metadata.c). */
#define EFS_META_HDR_SIZE     284
#define EFS_INODE_WIRE_SIZE   420
/* EFSM v6: inode row without name / tree rollups. Names live in a packed
 * dentry tail in the same inode-region blob. */
#define EFS_INODE_COMPACT_SIZE 124
#define EFS_CHUNK_WIRE_SIZE   120
#define EFS_ROLLUP_TOUCH      1
#define EFS_ROLLUP_CREATE     2
#define EFS_META_EFSM_V5      5
#define EFS_META_EFSM_V6      6
#define EFS_META_EFSM_V7      7
/* Current serialize (wire) version. v7 page-aligns the dentry region. */
#define EFS_META_VERSION      EFS_META_EFSM_V7

/* Inode-region dentry byte offset. v6 packs dentries immediately after the
 * compact inode rows, so appending one row memmoves the whole dentry tail and
 * re-dirties every page it spans (O(table) flush per create). v7 page-aligns
 * the dentry region: its offset depends only on inode_count, so a create that
 * does not cross a compact-page boundary leaves the dentry pages untouched
 * (O(1) flush). Deserialize recomputes the offset from the header's
 * inode_count, so no extra header field is needed. */
static inline size_t efs_meta_dent_off(uint32_t efsm_version, uint64_t inode_count)
{
    size_t off = (size_t)EFS_META_HDR_SIZE +
                 (size_t)inode_count * EFS_INODE_COMPACT_SIZE;
    if (efsm_version >= EFS_META_EFSM_V7)
        off = (off + (size_t)EFS_META_PAGE_SIZE - 1) &
              ~((size_t)EFS_META_PAGE_SIZE - 1);
    return off;
}

/* True if adding extra inode/chunk rows would exceed the v5 page caps. */
int efs_export_fits_page_cap(const struct efs_export *ex, uint64_t extra_inodes,
                             uint64_t extra_chunks);
/* Inode/chunk page counts for the current table (v6 compact accounting). */
void efs_export_meta_page_usage(const struct efs_export *ex,
                                uint32_t *ino_pages, uint32_t *chunk_pages);
uint32_t efs_export_shard_of(efs_ino_t ino, uint32_t shard_bits);
/* Phase 3b: shard that stores the chunk mapping for (ino, chunk_index).
 * bits==0 → 0. Independent of inode-row placement (shard_of(ino)). */
uint32_t efs_export_chunk_shard_of(efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t shard_bits);
/* Table that stores the chunk mapping (loads the shard on demand). */
struct efs_export *efs_export_table_for_chunk(struct efs_export *ex,
                                              efs_ino_t ino,
                                              uint32_t chunk_index);
/* Phase 3b: dentry shard for a spread directory. bits==0 → 0. */
uint32_t efs_export_dentry_shard_of(efs_ino_t parent, const char *name,
                                    uint32_t shard_bits);
/* Derived from rollups: imm_files + imm_dirs >= EFS_DIR_SPREAD_MIN. */
int efs_inode_dir_is_spread(const struct efs_inode *dir);
int efs_export_dir_is_spread(struct efs_export *ex, efs_ino_t dir);
/* Owner among live node ids (sorted or not). shard_count<=1 → lowest id
 * (today's meta primary). Else live[shard % nlive]. */
efs_node_id_t efs_shard_owner_of(uint32_t shard, uint32_t shard_count,
                                 const efs_node_id_t *live, uint32_t nlive);
/* Next inode for a create under parent. bits==0: next_ino++. bits>0:
 * allocate inside the parent's shard range. */
efs_ino_t efs_export_alloc_ino(struct efs_export *ex, efs_ino_t parent);
/* Allocate in `shard`'s congruence class from that shard table only.
 * The old main-table probe raced per-op CREATE (unlocked ino index while
 * shard 0 realloc'd) and is unnecessary once the child row and dentry
 * live on tables the caller has locked. */
efs_ino_t efs_export_alloc_ino_for_shard(struct efs_export *ex, uint32_t shard);
/* Peek the shard a create would land on. Files and directories stay on
 * the parent directory's shard so CREATE is local (no CREATE_SHARD RTT)
 * and rename of ecopy .tmp → final stays one-node. Independent trees
 * spread only when their parent inodes already live on different shards.
 * Does not advance create_rr. */
uint32_t efs_export_create_target(struct efs_export *ex, efs_ino_t parent,
                                  uint32_t mode, const char *name);
/* Table that owns `ino` (or parent for name ops). bits==0 → `ex`. */
struct efs_export *efs_export_table_for_ino(struct efs_export *ex, efs_ino_t ino);
struct efs_export *efs_export_table(struct efs_export *ex, uint32_t shard);
/* Existing shard table only — does not allocate. shard 0 / bits=0 → ex. */
struct efs_export *efs_export_shard_tab(struct efs_export *ex, uint32_t shard);
/* Blocker 2: pre-create every shard table so op/read paths never lazy-create
 * under a single shard lock. Caller holds the global lock or all shard locks. */
int efs_export_precreate_shards(struct efs_export *ex);
/* Kept for callers; does not instantiate extra-shard tables (descriptors
 * stay on the v8 root; the owner catchup loads a shard on demand). */
void efs_export_install_extra_roots(struct efs_export *ex);
/* Move inode/chunk rows into dest shards; keep dentries on the parent shard. */
int efs_export_rehash(struct efs_export *ex, uint32_t shard_bits);
/* Evict cold extra tables that are not dirty (reload from v8 extras). */
void efs_export_evict_cold_shards(struct efs_export *ex, uint32_t keep);
/* Ensure shard table exists (reinstall extra root if the cluster has one). */
int efs_export_load_shard(struct efs_export *ex, uint32_t shard);
static inline efs_ino_t efs_meta_shard_table_ino(uint32_t shard)
{
    return EFS_META_SHARD_INO_BASE + (efs_ino_t)shard;
}

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

/* Create a new inode, allocating the next sequential inode number.
 * Returns the inode number or 0 on error. */
efs_ino_t efs_export_create(struct efs_export *ex, efs_ino_t parent,
                              uint32_t mode, uid_t uid, gid_t gid, const char *name);

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

/* Remove one directory name. If it was the last hard link, also remove chunks. */
int efs_export_unlink_name(struct efs_export *ex, efs_ino_t parent, const char *name);
/* keep_last: drop the name but keep inode+chunks (open fds still exist). */
int efs_export_unlink_name_ex(struct efs_export *ex, efs_ino_t parent,
                              const char *name, int keep_last);
/* Drop a nlink=0 ghost inode + its chunks (last open fd closed). */
int efs_export_purge_unlinked(struct efs_export *ex, efs_ino_t ino);

/* Add a hard link (extra name) for an existing non-directory inode. */
int efs_export_link(struct efs_export *ex, efs_ino_t src_ino,
                    efs_ino_t new_parent, const char *new_name);

/* Cross-server hardlink helpers. nlink_inc/dec mutate every local row of
 * src_ino (canonical child table + any dentry copies). link_dentry writes
 * the new name on new_parent using src as the template; src.nlink is the
 * already-bumped value. */
int efs_export_nlink_inc(struct efs_export *ex, efs_ino_t src_ino,
                         struct efs_inode *out);
int efs_export_nlink_dec(struct efs_export *ex, efs_ino_t src_ino,
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
/* Same scan on one already-locked table (not every loaded shard tab). */
void efs_export_drop_chunks_table(struct efs_export *tab, efs_ino_t ino,
                                  uint32_t first_chunk);

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

/* Rebuild derived directory rollups from the inode table (after load/merge). */
void efs_export_recompute_rollups(struct efs_export *ex);

/* Format directory rollup fields into .stats text. Returns bytes written
 * (excluding NUL), or -1 if buf is too small / not a directory. */
int efs_export_format_stats(const struct efs_inode *dir, char *buf, size_t buflen);
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

/* Serialize export metadata to a memory buffer. Caller must free *buf.
 * v5 layout: fixed-size inode records, then chunk records. Optional
 * *ino_blob_len / *chunk_blob_len return the two-region split (header+inodes
 * vs chunks) so a flush can page them in disjoint index spaces. */
int efs_export_serialize(struct efs_export *ex, char **buf, size_t *len);
int efs_export_serialize_ex(struct efs_export *ex, char **buf, size_t *len,
                            uint32_t *ino_blob_len, uint32_t *chunk_blob_len);
/* Copy cache and rewrite dirty pages only. On success *used_incr=1 and
 * *buf is the new blob. If the cache layout cannot be reused (flush_full,
 * dent_off change, missing cache) *used_incr=0 and *buf is unchanged —
 * caller must efs_export_serialize_ex. omit_chunks skips the chunk region
 * (*chunk_blob_len = cache_chunk_len). */
int efs_export_serialize_dirty(struct efs_export *ex,
                               const char *cache, uint32_t cache_ino_len,
                               uint32_t cache_chunk_len, int omit_chunks,
                               char **buf, size_t *len,
                               uint32_t *ino_blob_len, uint32_t *chunk_blob_len,
                               int *used_incr);
void efs_export_flush_mark_full(struct efs_export *ex);
void efs_export_flush_mark_ino_slot(struct efs_export *ex, uint64_t slot);
void efs_export_flush_mark_chunk_slot(struct efs_export *ex, uint64_t slot);
void efs_export_flush_mark_dentry_tail(struct efs_export *ex);
void efs_export_flush_mark_dentry_all(struct efs_export *ex);
void efs_export_flush_clear_dirty(struct efs_export *ex);
int efs_export_flush_page_is_dirty(const struct efs_export *ex, uint32_t packed);
/* Copy inode/chunk rows (and header fields serialize needs) so the
 * O(table) pack can run without holding the server lock. Does not copy
 * indexes. Caller must efs_export_table_snapshot_free(snap). */
int efs_export_table_snapshot(const struct efs_export *ex,
                              struct efs_export *snap);
/* omit_chunks: keep chunk_count for the header but do not copy rows. */
int efs_export_table_snapshot_ex(const struct efs_export *ex,
                                 struct efs_export *snap, int omit_chunks);
void efs_export_table_snapshot_free(struct efs_export *snap);
/* Steal inode/chunk/index/child tables from src into dst (dst's old tables
 * are freed). Does not touch root, gm_blob, or shard_tabs. src is emptied. */
void efs_export_adopt_tables(struct efs_export *dst, struct efs_export *src);
void efs_export_pack_header(const struct efs_export *ex, uint8_t out[EFS_META_HDR_SIZE]);
void efs_export_pack_header_ver(const struct efs_export *ex,
                                uint8_t out[EFS_META_HDR_SIZE], uint32_t ver);
void efs_export_pack_inode(const struct efs_inode *ino, uint8_t out[EFS_INODE_WIRE_SIZE]);
void efs_export_pack_inode_compact(const struct efs_inode *ino,
                                   uint8_t out[EFS_INODE_COMPACT_SIZE]);
void efs_export_pack_chunk(const struct efs_chunk_entry *ce,
                           uint8_t out[EFS_CHUNK_WIRE_SIZE]);
int efs_export_inode_slot(struct efs_export *ex, efs_ino_t ino, uint64_t *slot);
int efs_export_chunk_slot(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                          uint64_t *slot);
int efs_export_needs_inode_grow(const struct efs_export *ex);
int efs_export_needs_chunk_grow(const struct efs_export *ex);
int efs_export_reserve_inodes(struct efs_export *ex, uint64_t extra);
int efs_export_reserve_chunks(struct efs_export *ex, uint64_t extra);

/* Deserialize export metadata, replacing current contents. */
int efs_export_deserialize(struct efs_export *ex, const char *buf, size_t len);

/* Merge a client's metadata update into the server's committed export.
 * For each inode and chunk entry in `inc`, the server's copy is updated when
 * the incoming entry is newer (higher mtime for inodes) or not yet present.
 * This lets concurrent clients commit without dropping each other's data. */
int efs_export_merge(struct efs_export *ex, const struct efs_export *inc);

/* Load from / save to a file. Fragmented exports save/load EFSR roots. */
int efs_export_load(struct efs_export *ex, const char *path);
int efs_export_save(struct efs_export *ex, const char *path);

/* --- Fragmented metadata (hybrid root + 2+1 pages) --- */

int efs_meta_blob_is_root(const char *buf, size_t len);
int efs_meta_blob_is_export(const char *buf, size_t len);

uint32_t efs_meta_page_count_for_blob(uint32_t blob_len);

/* Copy one zero-padded EFS_META_PAGE_SIZE page out of a serialized EFSM blob. */
int efs_meta_extract_page(const char *blob, uint32_t blob_len, uint32_t page_index,
                          uint8_t page_out[EFS_META_PAGE_SIZE]);

/* Assemble pages back into a blob of root->blob_len bytes. */
int efs_meta_assemble_blob(const struct efs_export_root *root,
                           const uint8_t pages[][EFS_META_PAGE_SIZE],
                           char **blob_out, size_t *blob_len_out);

int efs_export_root_serialize(const struct efs_export_root *root,
                              char **buf, size_t *len);
int efs_export_root_deserialize(struct efs_export_root *root,
                                const char *buf, size_t len);
/* Like deserialize, and reports how many prefix bytes were the EFSR
 * (so a trailing live EFSM can follow in the same GET_META payload). */
int efs_export_root_deserialize_used(struct efs_export_root *root,
                                     const char *buf, size_t len, size_t *used);

/* Fill root header from export (allocates page_checksums for page_count).
 * Two-region: pass the split lengths from efs_export_serialize_ex. A legacy
 * single-space flush can pass chunk_blob_len=0. */
int efs_export_root_prepare(struct efs_export_root *root,
                            const struct efs_export *ex,
                            uint64_t generation,
                            uint32_t ino_blob_len,
                            uint32_t chunk_blob_len);
int efs_export_root_capture_extras(struct efs_export_root *root,
                                   const struct efs_export *ex);

/* Max-merge prev's extra-shard descriptors into root: per shard id keep the
 * higher-generation descriptor and carry forward shards root lacks. Call
 * before adopting/committing any cluster root so a descriptor is never
 * dropped or regressed (an orphaned shard's tables are unrecoverable). */
int efs_export_root_maxmerge_extras(struct efs_export_root *root,
                                    const struct efs_export_root *prev);

/* Nonzero when both roots reference the same shard-0 pages (CoW checksums).
 * Used to recognize an extras-only root refresh: the sender contributed no
 * shard-0 content, so the receiver must not fence its live shard-0 table. */
int efs_export_root_same_pages(const struct efs_export_root *a,
                               const struct efs_export_root *b);

/* Merge incoming->extra_roots into the local shard tables without touching
 * shard-0. A descriptor is skipped when the local shard table is dirty
 * (unflushed ops would be fenced) or already holds an equal/newer generation
 * (single writer per shard => local is authoritative). Refreshes
 * ex->root.extra_roots from the merged tables. */
void efs_export_merge_extra_roots(struct efs_export *ex,
                                  const struct efs_export_root *incoming);

/* Free page_checksums; safe on zeroed roots. */
void efs_export_root_free(struct efs_export_root *root);

/* Move root contents into dst (steals page_checksums; clears src). */
void efs_export_root_move(struct efs_export_root *dst, struct efs_export_root *src);

/* Deep-copy root (including checksums). */
int efs_export_root_copy(struct efs_export_root *dst, const struct efs_export_root *src);

/* Pointer to checksums[page][frag] inside root->page_checksums. */
static inline uint8_t *efs_export_root_checksum(struct efs_export_root *root,
                                                uint32_t page, int frag)
{
    return root->page_checksums +
           ((size_t)page * EFS_NUM_FRAGMENTS + (size_t)frag) * EFS_HASH_SIZE;
}

static inline const uint8_t *efs_export_root_checksum_const(
    const struct efs_export_root *root, uint32_t page, int frag)
{
    return root->page_checksums +
           ((size_t)page * EFS_NUM_FRAGMENTS + (size_t)frag) * EFS_HASH_SIZE;
}

#endif
