#ifndef EFS_CLIENT_INTERNAL_H
#define EFS_CLIENT_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/opid.h"
#include <pthread.h>
#include <sys/types.h>

/* Chunk-buffer pool (bufpool.c): recycles <=EFS_CHUNK_SIZE buffers so the
 * dcache/rdcache/flush churn stays out of malloc (arena bloat added ~17 GB
 * of RSS across 200+ threads). Free with the SAME len passed at alloc. */
void *efs_buf_alloc(uint32_t len);
void efs_buf_free(void *p, uint32_t len);

struct efs_client {
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count;
    efs_node_id_t local_node_id;

    efs_export_id_t export_id;
    char export_name[EFS_MAX_NAME];
    struct efs_export export;
    pthread_mutex_t lock;
    /* Per-parent-directory stripes: create/unlink/chmod in different dirs
     * overlap. g_client.lock stays the rare table lock (realloc / snapshot). */
#define EFS_DIR_LOCKS 64
    pthread_mutex_t dir_lock[EFS_DIR_LOCKS];
    pthread_mutex_t dirty_mu; /* dirty-set growth + dirty-ops counter */
    pthread_mutex_t idx_mu;   /* inode/name/chunk index mutations */
    int dir_locks_ready;

    /* Inode allocation namespace. When non-zero, new inodes are allocated as
     * (ino_namespace | counter) so that concurrent clients never assign the
     * same inode number to different files (which would collide chunk
     * placements and corrupt data). Zero means allocate sequentially from
     * export.next_ino (single-client / test behaviour). */
    uint64_t ino_namespace;
    uint64_t ino_counter;
    /* Per-mount token for cluster flock / open-hold (not a POSIX lock owner). */
    uint64_t flock_token;
    /* Monotonic APPEND op-id seq (never 0). Retries of one reserve reuse it. */
    uint64_t append_opid_seq;
    /* I16 directory op-id (§7.9): per-mount identity, one monotonic seq
     * space, and the in-flight set that yields the contiguous ack the
     * server windows reclaim by. A slot is held from the first send of an
     * RPC to its final return; every retry inside reuses the same seq. */
#define EFS_OPID_INFLIGHT 512
    uint8_t opid_uuid[EFS_OPID_UUID_LEN]; /* guarded by inode_rpc.c opid_mu */
    uint32_t opid_epoch;
    uint64_t opid_next;                        /* next seq; 0 = unseeded */
    uint64_t opid_inflight[EFS_OPID_INFLIGHT]; /* 0 = free slot */

    /* Persistent connection pool to each server. The server's accept
     * loop handles multiple requests per connection; a pool of conns lets
     * FUSE workers and parallel fragment PUTs proceed without serializing
     * on a single fd. Each conn is TCP or TCP+RDMA (struct efs_conn).
     * Checkout waits if all slots are busy. */
    struct efs_conn *conn[EFS_MAX_NODES][EFS_CLIENT_CONNS_PER_NODE];
    int conn_busy[EFS_MAX_NODES][EFS_CLIENT_CONNS_PER_NODE];
    pthread_mutex_t conn_lock[EFS_MAX_NODES];
    pthread_cond_t conn_cv[EFS_MAX_NODES];
    int conn_pool_size; /* 1..EFS_CLIENT_CONNS_PER_NODE, from env or default */

    /* Soft down-mark: after consecutive failures, skip connect attempts for a
     * cooldown so one dead peer cannot stall every chunk PUT (2+1 quorum). */
    int node_fail_streak[EFS_MAX_NODES];
    int64_t node_down_until_ms[EFS_MAX_NODES]; /* CLOCK_MONOTONIC ms; 0 = up */

    /* When non-zero, metadata replication is coalesced: changes only mark
     * dirty and flush every meta_batch_ops operations (or when forced on
     * unmount). Flushes pack the full export into 2+1 pages and push a tiny
     * EFSR root (≥2 acks). C tests leave this at 0 for immediate sync. */
    int meta_batch;
    uint32_t meta_batch_ops; /* flush threshold; 0 → default */
    uint32_t meta_dirty_ops;
    int meta_dirty;

    /* Dirty tracking for batched meta flushes (meta_batch only).
     * Inodes: open-addressing set (key 0 = empty).
     * Chunks: set for dedup + parallel arrays of (ino, chunk_index). */
    uint64_t *dirty_ino_keys;
    uint64_t dirty_ino_mask;
    uint64_t dirty_ino_count;
    uint64_t *dirty_chunk_keys;
    uint64_t dirty_chunk_mask;
    efs_ino_t *dirty_chunk_inos;
    uint32_t *dirty_chunk_idxs;
    uint64_t dirty_chunk_count;
    uint64_t dirty_chunk_cap;

    /* Dedicated metadata-flush thread: batched threshold flushes run here so
     * the O(table) serialize/encode/PUT work never executes on a FUSE worker
     * thread. note_meta_change() only sets flush_req and signals. */
    pthread_t meta_flush_tid;
    pthread_mutex_t meta_flush_mu;
    pthread_cond_t meta_flush_cv;
    int meta_flush_req;
    int meta_flush_stop;
    int meta_flush_started;
    int meta_flush_force; /* next flush does a full serialize */
    /* Set once this process commits a v7 (page-aligned dentry) generation.
     * Until then the first flush is forced full so every on-disk page is
     * rewritten in v7 layout — the mounted cache may sit on v6 pages even
     * when the adopted blob reads v7 (GET_META re-serializes server-side). */
    int meta_v7_committed;
    int meta_cap_blocked;  /* last flush hit the page cap; skip until shrink */
    int last_err;          /* EFS_ERR_* from the last mutating client op */
    int write_readonly;    /* mount is read-only (no write path enabled) */
    uint32_t dirty_stripe_ops[EFS_DIR_LOCKS];
    int last_dirty_stripe;

    /* Phase 2b: sticky flag set when a dirty-report RPC fails (so fsync can
     * report EIO). The dirty set itself is the pending-report queue (reused
     * from the crash-safe rebase); report_dirty clears it on success. */
    int report_flush_failed;
};

/* Mark inode/chunk dirty for the next batched metadata delta flush.
 * No-ops when meta_batch is disabled. Takes dirty_mu internally. */
void efs_client_mark_ino_dirty(efs_ino_t ino);
int efs_client_ino_is_dirty(efs_ino_t ino);
/* Explicit utimens: data-path mtime bumps and REPORT echoes must not
 * put "now" back over a user-set (possibly older) mtime. */
void efs_client_mtime_pin(efs_ino_t ino);
void efs_client_mtime_unpin(efs_ino_t ino);
int efs_client_mtime_is_pinned(efs_ino_t ino);
void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t chunk_index);
int efs_client_ensure_meta_room(uint64_t extra_inodes, uint64_t extra_chunks);

/* Staging-table eviction (client-cache design Part A): the evictor thread
 * bounds g_client.export to EFS_CLIENT_META_MB (default 256) by dropping
 * clean, closed, unreported rows + chunk recs (LRU by last touch). */
void efs_client_stage_evict_start(void);
void efs_client_stage_evict_stop(void);
/* Register the FUSE-layer pin providers (open fd, byte-range lock) before
 * the evictor thread starts. stage_evict.c is linked into non-FUSE binaries,
 * so it must not reference efs_fuse.c symbols directly. */
void efs_client_stage_set_pin_hooks(int (*is_open)(efs_ino_t),
                                    int (*has_plock)(efs_ino_t));
void efs_client_stage_evict_kick(void);
/* In-flight op pin (design rule 3, and a live append reservation).
 * Balanced pin/unpin around the op. Takes only its own leaf lock. */
void efs_client_stage_pin(efs_ino_t ino);
void efs_client_stage_unpin(efs_ino_t ino);
/* Targeted drop of one ino (ghost reclaim at last close). Takes the table
 * locks itself; safe to call from any FUSE handler. */
void efs_client_stage_evict_ino(efs_ino_t ino);
/* LRU touch — takes the leaf LRU lock internally (safe under the table
 * locks). Call when a row is staged or used. */
void efs_client_stage_touch(efs_ino_t ino);
/* Pin queries used by the evictor (also usable elsewhere). */
int efs_client_ino_is_dirty_locked(efs_ino_t ino); /* caller holds dirty_mu */
int efs_client_ino_is_open(efs_ino_t ino);
int efs_client_ino_has_plock(efs_ino_t ino);
int efs_dcache_ino_pinned(efs_ino_t ino);
int efs_client_rpc_lookup(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, struct efs_inode *out);
int efs_client_rpc_lookup_path(efs_export_id_t export_id, efs_ino_t start,
                               const char *path, uint32_t flags,
                               struct efs_msg_inode_lookup_path_reply *out);
int efs_client_rpc_create(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, uint32_t mode, uid_t uid, gid_t gid,
                          uint32_t flags, efs_ino_t *out_ino,
                          struct efs_inode *out);
int efs_client_rpc_getattr(efs_export_id_t export_id, efs_ino_t ino,
                           struct efs_inode *out);
/* Local-first getattr (open fd). RPC only on a table miss. */
int efs_client_stat_ino(efs_ino_t ino, struct efs_inode *out);
/* Open fd: local size, server nlink. A peer unlink is invisible in the
 * local row; copying the whole GETATTR row would replace an unflushed
 * size. RPC failure keeps the local ghost. */
int efs_client_stat_open(efs_ino_t ino, struct efs_inode *out);
/* Path getattr: GETATTR RPC + adopt + overlay. Peer size/nlink growth. */
int efs_client_stat_refresh(efs_ino_t ino, struct efs_inode *out);
/* Local table only — no RPC. For parent-dir checks on a known nodeid. */
int efs_client_stat_local(efs_ino_t ino, struct efs_inode *out);
/* Name this client already dual-applied. A miss is not "does not exist". */
int efs_client_lookup_local(efs_ino_t parent, const char *name,
                            struct efs_inode *out);
/* Adopt a LOOKUP/GETATTR reply and overlay this client's unflushed size. */
void efs_client_adopt_lookup(const struct efs_inode *rpc, struct efs_inode *out);
/* Readdir with the server's (src, name) resume cookie. */
int efs_client_rpc_readdir_cur(efs_export_id_t export_id, efs_ino_t parent,
                               struct efs_inode *ents, uint32_t *inout_count,
                               uint32_t *src_io, char *name_io,
                               uint32_t *done_out);
struct efs_chunk_rec;
int efs_client_rpc_getchunks(efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count);
/* Returns EFS_OK only if every GETCHUNKS in the range succeeded; on an
 * error the local table may be PARTIAL for the range. */
int efs_client_pull_chunks_range(efs_ino_t ino, uint32_t start_ci,
                                 uint32_t end_ci);
int efs_dcache_replay_stale(efs_ino_t ino, uint32_t ci);
int efs_client_rpc_unlink(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, int is_dir);
int efs_client_rpc_rename_at(efs_export_id_t export_id, efs_ino_t old_parent,
                             const char *old_name, efs_ino_t new_parent,
                             const char *new_name, struct efs_inode *out);
int efs_client_rpc_setattr(efs_export_id_t export_id, efs_ino_t ino,
                           uint32_t mask, uint32_t mode, uid_t uid, gid_t gid,
                           uint64_t size, uint64_t mtime, uint32_t mtime_nsec,
                           uint64_t atime, struct efs_inode *out);
/* op is EFS_XATTR_GET/SET/REMOVE/LIST. out_len is the buffer size in and
 * the byte count out. SET/REMOVE may pass out NULL. */
int efs_client_rpc_xattr(efs_export_id_t export_id, efs_ino_t ino, uint8_t op,
                         uint32_t flags, const void *name, uint16_t nlen,
                         const void *val, uint32_t vlen, void *out,
                         uint32_t *out_len);
int efs_client_rpc_link(efs_export_id_t export_id, efs_ino_t src_ino,
                        efs_ino_t new_parent, const char *new_name,
                        struct efs_inode *out);
/* Cross-client O_APPEND reservation. end = reserved+len (reply size);
 * start is reply atime when the host stamped this reservation. */
int efs_client_rpc_append_reserve(efs_export_id_t export_id, efs_ino_t ino,
                                  uint64_t len, uint64_t seq,
                                  uint64_t *new_size_out, uint64_t *start_out);
int efs_client_rpc_hold(efs_export_id_t export_id, efs_ino_t ino, int open,
                        uint64_t owner);
int efs_client_rpc_flock(efs_export_id_t export_id, efs_ino_t ino, uint32_t op,
                         uint64_t owner);
/* Byte-range fcntl (EFS_FLOCK_FCNTL). start/end are half-open. reply_out
 * is filled for GETLK (nlink=type, size=start, ctime=end). */
int efs_client_rpc_flock_range(efs_export_id_t export_id, efs_ino_t ino,
                               uint32_t op, uint64_t owner, uint64_t start,
                               uint64_t end, struct efs_msg_inode_reply *reply_out);
/* Report dirty metadata (chunk mappings + inode size/mtime) as ONE batch
 * to a voter of ALL groups (a "dual-host"). A batch's recs span inode
 * groups and lane groups, and only a node voting in every group can apply
 * the whole batch locally. sync=1 is the fsync durability barrier. */
struct efs_chunk_rec;
struct efs_ino_size_rec;
/* W17.1: monotonic ms. 0 clears. rpc_send_recv_dual stops before the
 * next attempt once this passes. One in-flight recv still finishes. */
void efs_client_rpc_set_deadline_ms(uint64_t mono_ms);
int efs_client_rpc_report_dirty_raft(efs_export_id_t export_id,
                                     const struct efs_chunk_rec *recs,
                                     uint32_t count,
                                     const struct efs_ino_size_rec *irecs,
                                     uint32_t ino_count, int sync);
/* Phase 2b: snapshot the dirty set and report it to the primary (the flush
 * mechanism that replaces the blob flush). sync=1 = fsync barrier. */
int efs_client_report_dirty(int sync);
int efs_client_report_dirty_ino(efs_ino_t only_ino, int sync);
/* Read-miss resolution: a chunk row missing from the local table inside
 * the file size is a HOLE only if the owner says so. Pulls the mappings
 * for [ci0, ci1) and returns EFS_OK when the range is now authoritative
 * (whatever is still missing is a real hole); any error means the caller
 * must NOT zero-fill (return the error instead — I9, never substitute
 * absent for unavailable). A successful pull covering the same range is
 * reused for 1 s so repeated hole reads cost one GETCHUNKS, not one per
 * read. */
int efs_client_pull_layout_miss(efs_ino_t ino, uint32_t ci0, uint32_t ci1);

void efs_client_ensure_dir_locks(void);
void efs_client_lock_dir(efs_ino_t parent);
void efs_client_unlock_dir(efs_ino_t parent);
void efs_client_lock_dirs2(efs_ino_t a, efs_ino_t b);
void efs_client_unlock_dirs2(efs_ino_t a, efs_ino_t b);
void efs_client_lock_all_dirs(void);
void efs_client_unlock_all_dirs(void);
/* Table lock + every dir stripe: realloc / export-wide snapshot. */
void efs_client_table_lock(void);
int efs_client_set_chunk(struct efs_export *ex, efs_ino_t ino,
                         uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);
void efs_client_table_unlock(void);

extern struct efs_client g_client;

/* Resolve a path to an inode number. Returns 0 on success. */
int efs_client_lookup(const char *path, struct efs_inode *out);

/* Set atime and mtime in one SETATTR RPC. */
int efs_client_utimens_both(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec,
                            uint64_t atime);

/* Client inode-number namespace so concurrent writers do not collide. */
void efs_client_setup_ino_namespace(void);

/* Record a local metadata mutation. With meta_batch==0 this replicates
 * immediately (test / C-API behaviour). With meta_batch!=0 it coalesces
 * until meta_batch_ops changes accumulate (or force!=0). */
int efs_client_note_meta_change(int force);
/* Wake the background reporter; do not wait. Close-time size publish
 * used to block every FUSE flush on REPORT_CHUNKS (unlink-storm create
 * ran at ~4 files/s). posix2 still sees the report: B starts only after
 * A's SSH step returns, which is far longer than one async REPORT. */
void efs_client_kick_meta_flush(void);
void efs_client_stop_meta_flush(void);

/* Decoded-chunk cache for sub-chunk reads and RMW. */
int efs_rdcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len);
/* Copy [off, off+len) from a cached chunk. 0 = hit. Avoids a 128 KiB malloc
 * on the 4k random-read path. */
int efs_rdcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                     uint8_t *dst, uint32_t len);
void efs_rdcache_put(efs_ino_t ino, uint32_t ci, const uint8_t *src, uint32_t len);
void efs_rdcache_invalidate(efs_ino_t ino, uint32_t ci);

/* Dirty assembled chunks: combine partial writes and PUT on flush/evict. */
int efs_dcache_has(efs_ino_t ino, uint32_t ci);
int efs_dcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len);
int efs_dcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                    uint8_t *dst, uint32_t len);
/* Same, but only unpublished dirty ranges (not have_base=1). Unlink-open
 * ghosts and size-0 stubs use this so a published cached chunk cannot
 * outrun inode.size under FOPEN_DIRECT_IO. */
int efs_dcache_copy_unpub(efs_ino_t ino, uint32_t ci, uint32_t off,
                          uint8_t *dst, uint32_t len);
/* have_base image with no generation check. Only for an inode row that
 * is already gone (unlink-open): a span leaves base gen 0, so
 * efs_dcache_copy refuses the image, and the bytes live only here. */
int efs_dcache_copy_kept(efs_ino_t ino, uint32_t ci, uint32_t off,
                         uint8_t *dst, uint32_t len);
/* Fill buf with the published chunk (fragment GET). Does not consult
 * inode.size. Unpublished → zeros + EFS_OK. */
int efs_client_fetch_published_chunk(efs_ino_t ino, uint32_t ci,
                                     uint8_t *buf, uint32_t len);
/* Overlay dirty dcache bytes onto a fetched/zero chunk (have_base=0 ranges). */
void efs_dcache_overlay(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len);
int efs_dcache_flush_all(void);
int efs_dcache_flush_ino(efs_ino_t ino);
void efs_dcache_reclaim_stop(void);
void efs_dcache_drop(efs_ino_t ino, uint32_t ci);
/* Drop a cached chunk only if it is not dirty (peer layout change). */
void efs_dcache_drop_if_clean(efs_ino_t ino, uint32_t ci);
/* Patch a cached chunk, loading it (rdcache / servers / zeros) on miss.
 * 0 = cached (size updated if the write grew the file), -1 = cannot cache. */
int efs_dcache_try_patch(efs_ino_t ino, uint64_t offset, uint32_t len,
                         const uint8_t *src);
/* Whole 128 KiB chunk. `chunk` is efs_buf_alloc'd; stolen on success. */
int efs_dcache_store_full_owned(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                                uint32_t cs);
/* Clear a dcache present_extra count before the chunk enters the table. */
void efs_dcache_yield_extra(efs_ino_t ino, uint32_t ci);
/* O_APPEND: range-track until the first PUT, then keep have_base=1.
 * Flush merge-base is efs_client_fetch_published_chunk (not size-clamped
 * efs_client_read). */
int efs_dcache_try_patch_sparse(efs_ino_t ino, uint64_t offset, uint32_t len,
                                const uint8_t *src);
/* If dirty assembled chunks exceed the cap, PUT them now. */
void efs_dcache_init(void);
void efs_dcache_maybe_reclaim(void);

/* Enable coalesced metadata replication for the FUSE client. */
void efs_client_enable_meta_batch(uint32_t every_n_ops);

/* Fetch a fragment from a node. data must hold at least expected_frag_len bytes.
 * Returns 0 on success. */
int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, uint32_t expected_frag_len,
                            uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE],
                            uint64_t chunk_generation);

/* Store a fragment on a node. Returns 0 on success. */
/* Read bytes from a file. Returns 0 on success. */
int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len);
void efs_client_read_pools_stop(void);

/* Write bytes to a file and replicate metadata. Returns 0 on success. */
int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf);

/* Write bytes without replicating metadata. The caller must replicate later
   (e.g. on FUSE release). */
int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf);

/* Create a file or directory. Returns the inode number or 0 on error.
 * flags: EFS_CREATE_F_HOLD to increment the server open-hold in the
 * same CREATE RPC (FUSE create+open). */
efs_ino_t efs_client_create(efs_ino_t parent, const char *name, uint32_t mode,
                            uid_t uid, gid_t gid);
efs_ino_t efs_client_create_ex(efs_ino_t parent, const char *name, uint32_t mode,
                               uid_t uid, gid_t gid, uint32_t flags);

/* Remove a file or directory. */
int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir);

/* Add a hard link to an existing inode under new_parent/new_name. */
int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name);

/* Set mode bits. */
int efs_client_chmod(efs_ino_t ino, uint32_t mode);

/* Set owner/group. */
int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid);

/* Set modification time (seconds; nsec cleared). */
/* Set modification time with nanoseconds (for utimensat / rsync). */
int efs_client_utimens(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec);

/* Set access time (seconds; never bumped on read). */
int efs_client_set_atime(efs_ino_t ino, uint64_t atime);

/* Truncate or extend a file to the given size. */
int efs_client_truncate(efs_ino_t ino, uint64_t size);

/* Rename/move an inode to a new parent and name. */
int efs_client_rename_at(efs_ino_t ino, efs_ino_t old_parent, const char *old_name,
                         efs_ino_t new_parent, const char *new_name);

/* Initialize node cache from the list of nodes. */
void efs_client_init_nodes(struct efs_client *c, const char *node_list[EFS_MAX_NODES],
                           uint32_t node_count);

/* Discover the full cluster membership by asking one server. Returns 0 on success. */
int efs_client_discover_nodes(struct efs_client *c, const char *host, uint16_t port);

/* Borrow a live conn for node_id (1-based). Blocks until a pool slot is
 * free. Caller must efs_client_conn_release(node_id, conn) or _drop(...). */
struct efs_conn *efs_client_conn_get(efs_node_id_t node_id);

/* Return a healthy connection to the pool. */
void efs_client_conn_release(efs_node_id_t node_id, struct efs_conn *conn);

/* Close a broken connection and free its pool slot. Also closes other
 * idle pooled conns to the same node (they are often stale after an
 * efsd restart). */
void efs_client_conn_drop(efs_node_id_t node_id, struct efs_conn *conn);

/* Close all idle pooled sockets to node_id so the next checkout reconnects.
 * In-flight (busy) sockets are left alone until their owners drop/release. */
void efs_client_conn_invalidate_node(efs_node_id_t node_id);

/* Track peer liveness for the down-mark (connect / I/O success or failure). */
void efs_client_node_note_ok(efs_node_id_t node_id);
void efs_client_node_note_fail(efs_node_id_t node_id);
/* True while the peer is in the short connect-skip cooldown. */
int efs_client_node_is_down(efs_node_id_t node_id);
/* Clear every down-mark so the next checkout re-probes. Call when fail-fast
 * would otherwise spin forever without ever calling conn_get. */
void efs_client_nodes_force_reprobe(void);

/* Initialize the connection pool (call once after node_count is known). */
void efs_client_conn_init(void);

/* Close pooled sockets, free dirty-tracking / export memory, destroy locks.
 * Safe to call once from FUSE destroy / process exit. */
void efs_client_shutdown(void);

/* PUT all three fragments concurrently (send-all / recv-all on three
 * pooled sockets). Returns EFS_OK if quorum (>=2) acks, else an error. */
int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);

#endif
