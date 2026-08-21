#ifndef EFS_CLIENT_INTERNAL_H
#define EFS_CLIENT_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/network.h"
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

    /* Last successful EFSM blob (point 2): patch dirty rows instead of
     * re-serializing the whole table on a batched flush. */
    char *meta_cache_blob;      /* header + inode rows */
    size_t meta_cache_cap;
    char *meta_cache_ch;        /* chunk rows (separate so ino growth does not memmove) */
    size_t meta_cache_ch_cap;
    size_t meta_cache_len;
    uint32_t meta_cache_ino_len;
    uint32_t meta_cache_ch_len;
    uint64_t meta_cache_icount;
    uint64_t meta_cache_ccount;
    uint64_t meta_cache_epoch;

    /* Inode allocation namespace. When non-zero, new inodes are allocated as
     * (ino_namespace | counter) so that concurrent clients never assign the
     * same inode number to different files (which would collide chunk
     * placements and corrupt data). Zero means allocate sequentially from
     * export.next_ino (single-client / test behaviour). */
    uint64_t ino_namespace;
    uint64_t ino_counter;

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

    /* Dirty-page flush: per dual-slot parity (generation & 1), the page
     * content hashes AND fragment checksums committed by the last successful
     * flush of that parity. A page whose content hash still matches its
     * parity slot is already durably stored at the same chunk index (slots
     * alternate per gen; the server GC keeps in-range fragments), so its
     * fragment PUT is skipped and the committed fragment checksums are
     * reused verbatim — the flush's network cost becomes O(dirty pages),
     * not O(table). Only touched by the flush path (g_repl_mu serializes). */
    /* Logical-page skip tables (index = inode pi, or CHUNK_PAGE_BASE+pj).
     * Sized EFS_META_MAX_PAGES so growing the inode region cannot shift
     * chunk-page slots. */
    uint8_t *meta_slot_hashes[2];
    uint8_t *meta_slot_sums[2];
    uint32_t meta_slot_ino_pages[2];
    uint32_t meta_slot_chunk_pages[2];
    uint32_t meta_slot_pages[2]; /* ino+chunk; kept for cleanup/compat */

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
    /* After a hunted/skipped reconstruct, rewrite chunk-table pages to
     * canonical CIs (same-parity gen+2) so the next mount does not hunt. */
    int meta_heal;
    uint32_t meta_heal_skipped;
    int meta_heal_pending; /* heal requested; run when dirty set is idle */
    int meta_cap_blocked;  /* last flush hit the page cap; skip until shrink */
    int last_err;          /* EFS_ERR_* from the last mutating client op */
    int write_readonly;    /* another client holds the EFSR write lease */
    uint64_t write_lease_id;
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
void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t chunk_index);
int efs_client_ensure_meta_room(uint64_t extra_inodes, uint64_t extra_chunks);
int efs_client_take_write_lease(void);
int efs_client_rpc_lookup(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, struct efs_inode *out);
int efs_client_rpc_create(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, uint32_t mode, uid_t uid, gid_t gid,
                          efs_ino_t *out_ino, struct efs_inode *out);
int efs_client_rpc_getattr(efs_export_id_t export_id, efs_ino_t ino,
                           struct efs_inode *out);
int efs_client_rpc_readdir(efs_export_id_t export_id, efs_ino_t parent,
                           struct efs_inode *ents, uint32_t *inout_count,
                           uint32_t start);
struct efs_chunk_rec;
int efs_client_rpc_getchunks(efs_export_id_t export_id, efs_ino_t ino,
                             uint32_t start, struct efs_chunk_rec *recs,
                             uint32_t *inout_count);
int efs_client_rpc_unlink(efs_export_id_t export_id, efs_ino_t parent,
                          const char *name, int is_dir);
int efs_client_rpc_rename(efs_export_id_t export_id, efs_ino_t ino,
                          efs_ino_t new_parent, const char *new_name,
                          struct efs_inode *out);
int efs_client_rpc_setattr(efs_export_id_t export_id, efs_ino_t ino,
                           uint32_t mask, uint32_t mode, uid_t uid, gid_t gid,
                           uint64_t size, uint64_t mtime, uint32_t mtime_nsec,
                           uint64_t atime, struct efs_inode *out);
int efs_client_rpc_link(efs_export_id_t export_id, efs_ino_t src_ino,
                        efs_ino_t new_parent, const char *new_name,
                        struct efs_inode *out);
/* Phase 2b: report dirty metadata (chunk mappings + inode size/mtime) to the
 * metadata primary, replacing the client blob flush. sync=1 makes the primary
 * commit the export before replying (the fsync durability barrier). */
struct efs_chunk_rec;
struct efs_ino_size_rec;
int efs_client_rpc_report_dirty(efs_export_id_t export_id,
                                const struct efs_chunk_rec *recs,
                                uint32_t count,
                                const struct efs_ino_size_rec *irecs,
                                uint32_t ino_count, int sync);
/* Phase 2b: snapshot the dirty set and report it to the primary (the flush
 * mechanism that replaces the blob flush). sync=1 = fsync barrier. */
int efs_client_report_dirty(int sync);
int efs_client_load_shard(uint32_t shard);

void efs_client_ensure_dir_locks(void);
void efs_client_lock_dir(efs_ino_t parent);
void efs_client_unlock_dir(efs_ino_t parent);
void efs_client_lock_dirs2(efs_ino_t a, efs_ino_t b);
void efs_client_unlock_dirs2(efs_ino_t a, efs_ino_t b);
void efs_client_lock_all_dirs(void);
void efs_client_unlock_all_dirs(void);
/* Table lock + every dir stripe: realloc / export-wide snapshot. */
void efs_client_table_lock(void);
void efs_client_table_unlock(void);

/* Seal a staged small file into its parent directory pack (FUSE release). */
int efs_client_pack_seal(efs_ino_t ino);
void efs_client_pack_flush_all(void);
/* 0 = this ino is staged (out_len set); -1 = not in the pack stage. */
int efs_client_pack_stage_read(efs_ino_t ino, uint64_t offset, size_t size,
                               char *buf, size_t *out_len);
/* 0 = hit the in-memory dir pack tail (not yet PUT); -1 = miss. */
int efs_client_dir_pack_read(efs_ino_t pack_ino, uint64_t offset, size_t size,
                             char *buf, size_t *out_len);

extern struct efs_client g_client;

/* Resolve a path to an inode number. Returns 0 on success. */
int efs_client_lookup(const char *path, struct efs_inode *out);

/* Fetch full metadata from a server and replace local copy. */
int efs_client_fetch_metadata(const char *host, uint16_t port);

/* Result of the network-only metadata fetch phase. efsm is malloc'd and
 * owned by the caller; root is valid when have_root is set. */
struct efs_meta_fetch {
    struct efs_export_root root;
    int have_root;
    int saw_bootstrap;
    int fetch_ok;
    int last_rc;
    uint32_t bootstrap_id; /* export id carried by the bootstrap root shell */
    char *efsm;
    size_t efsm_len;
};

/* Network-only metadata fetch: newest EFSR across nodes + assembled EFSM
 * blob when available. Takes no locks and does not touch g_client, so a
 * caller can swap tables under its own critical section (STALE resync). */
int efs_client_fetch_meta_best(const char *host, uint16_t port,
                               struct efs_meta_fetch *f);

/* Replicate local metadata to all servers. Returns number of acks. */
/* Must be called without g_client.lock held; it takes the lock only to
 * serialize, then releases it for the duration of the network I/O. */
int efs_client_replicate_metadata(void);
int efs_client_sync_meta(void);
/* Take ownership of a tight EFSM blob (hdr+inodes+chunks) as the
 * incremental cache. 0 = adopted (caller must not free); -1 = too small. */
int efs_client_meta_cache_adopt(char *blob, size_t blob_len);

/* Record a local metadata mutation. With meta_batch==0 this replicates
 * immediately (test / C-API behaviour). With meta_batch!=0 it coalesces
 * until meta_batch_ops changes accumulate (or force!=0). */
int efs_client_note_meta_change(int force);
/* Background: PUT recovered chunk-table pages at v5 indexes and publish. */
void efs_client_schedule_meta_heal(void);
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
/* If dirty assembled chunks exceed the cap, PUT them now. */
void efs_dcache_maybe_reclaim(void);

/* Enable coalesced metadata replication for the FUSE client. */
void efs_client_enable_meta_batch(uint32_t every_n_ops);

/* Fetch a fragment from a node. data must hold at least expected_frag_len bytes.
 * Returns 0 on success. */
int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, uint32_t expected_frag_len,
                            uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE]);

/* Store a fragment on a node. Returns 0 on success. */
int efs_client_put_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, const uint8_t *data, uint32_t frag_len,
                            const uint8_t checksum[EFS_HASH_SIZE]);

/* Read bytes from a file. Returns 0 on success. */
int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len);

/* Write bytes to a file and replicate metadata. Returns 0 on success. */
int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf);

/* Write bytes without replicating metadata. The caller must replicate later
   (e.g. on FUSE release). */
int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf);

/* Create a file or directory. Returns the inode number or 0 on error. */
efs_ino_t efs_client_create(efs_ino_t parent, const char *name, uint32_t mode,
                            uid_t uid, gid_t gid);

/* Remove a file or directory. */
int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir);

/* Add a hard link to an existing inode under new_parent/new_name. */
int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name);

/* Set mode bits. */
int efs_client_chmod(efs_ino_t ino, uint32_t mode);

/* Set owner/group. */
int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid);

/* Set modification time (seconds; nsec cleared). */
int efs_client_utime(efs_ino_t ino, uint64_t mtime);

/* Set modification time with nanoseconds (for utimensat / rsync). */
int efs_client_utimens(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec);

/* Set access time (seconds; never bumped on read). */
int efs_client_set_atime(efs_ino_t ino, uint64_t atime);

/* Truncate or extend a file to the given size. */
int efs_client_truncate(efs_ino_t ino, uint64_t size);

/* Rename/move an inode to a new parent and name. */
int efs_client_rename(efs_ino_t ino, efs_ino_t new_parent, const char *new_name);

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
