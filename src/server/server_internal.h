#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <pthread.h>

/* Soft cap on concurrent accept/handler threads. Excess sockets are closed
 * immediately so one connection storm cannot exhaust RLIMIT_NOFILE. */
#define EFS_SERVER_MAX_CONNS 4096

/* Persistent server→server TCP pool (meta/migrate/heartbeat/status). */
void server_peer_pool_init(void);
void server_peer_pool_shutdown(void);
/* Checkout a live fd to host:port (connects on miss). Returns -1 on failure. */
int server_peer_conn_get(const char *host, uint16_t port);
/* Return fd to the pool after a successful request/response. */
void server_peer_conn_release(const char *host, uint16_t port, int fd);
/* Close and discard a broken fd (net/protocol error). */
void server_peer_conn_drop(const char *host, uint16_t port, int fd);
/* Shared PUT writer pool (one queue, all --storage paths). --writers n is
 * the total; n=0 is inline; n<0 (startup default) means auto from nproc. */
#define EFS_WRITERS_RESERVED         4  /* main + heartbeat + migrate + catchup */
#define EFS_MAX_WRITERS              64
#define EFS_MAX_WRITERS_PER_PATH     EFS_MAX_WRITERS /* CLI / bench alias */
#define EFS_DEFAULT_WRITERS          8               /* bench fallback */
#define EFS_WRITERS_PER_PATH_DEFAULT EFS_DEFAULT_WRITERS

enum efsd_server_state {
    SERVER_STATE_ACTIVE = 0,
    SERVER_STATE_LEAVING = 1,   /* leave cluster after successful remove (no migrate) */
    SERVER_STATE_SHRINKING = 2, /* reducing local quota usage */
    SERVER_STATE_DRAINING = 3,  /* migrating local fragments to peers */
    SERVER_STATE_DRAINED = 4,   /* empty; rejects new PUTs until undrain or remove */
};

/* Max queued meta-flush contenders per export (well above expected client
 * count; overflow writers simply get BUSY and re-BEGIN, re-entering). */
#define EFS_META_WRITER_QMAX 32

/* RAM-only open-fd + flock state. Never serialized. */
struct efs_ino_hold {
    efs_export_id_t eid;
    efs_ino_t ino;
    uint32_t refs;
    uint64_t flock_owner[8];
    uint8_t flock_ex[8];
    uint32_t flock_n;
    struct efs_ino_hold *next;
};

struct efsd_server {
    efs_node_id_t id;
    char addr[64];
    uint16_t port;

    /* Export lifetime: handlers/writers hold a use-count while doing I/O so
     * server_destroy_export cannot free/compact the slot out from under them.
     * Guarded by s->lock; destroy sets destroying then waits on export_idle_cv
     * until export_inflight drains to 0. */
    uint32_t export_inflight[EFS_MAX_EXPORTS];
    uint8_t export_destroying[EFS_MAX_EXPORTS];
    pthread_cond_t export_idle_cv;
    /* Local data roots: 1..EFS_MAX_STORAGE_PATHS (full-fragment stripe).
     * storage_path is always storage_paths[0] for on-disk paths (log/PID/meta).
     * Cluster advertise (efs_node / HELLO) uses server_format_storage_paths(). */
    char storage_paths[EFS_MAX_STORAGE_PATHS][EFS_MAX_PATH];
    uint32_t storage_path_count;
    char storage_path[EFS_MAX_PATH];
    uint64_t quota; /* local storage quota in bytes; 0 = unlimited */

    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count;

    struct efs_export exports[EFS_MAX_EXPORTS];
    uint32_t export_count;
    uint32_t epoch;

    pthread_mutex_t lock;
    int listen_fd;
    int running;

    int state; /* enum efsd_server_state */
    uint64_t shrink_target; /* target used bytes after shrink-quota migration */
    pthread_t migrate_tid;
    pthread_t meta_catchup_tid; /* background meta rebuild + local heal */
    pthread_t rejoin_tid; /* background rejoin retry thread */
    char rejoin_addr[64]; /* explicit --join target to keep retrying; empty = use persisted peers */
    uint16_t rejoin_port;

    int direct_io; /* use O_DIRECT for fragment reads/writes */
    /* Shared writer-pool size (0 = inline, <0 = auto from nproc at start). */
    int nwriters;
    int persist_nodes; /* persist cluster membership to disk */
    int perf; /* run under perf record when starting */
    int export_meta_dirty; /* defer metadata.bin writes across PUT_META */
    int usage_dirty; /* local->used changed; flush meta/usage.bin soon */
    int nodes_dirty; /* membership changed; persist nodes.bin off the lock */

    /* Incremental meta-rebuild cache (per export slot): the assembled EFSM
     * blob from the last successful rebuild plus that generation's page
     * checksums. Pages whose checksums are unchanged in a new root are
     * memcpy'd from the cache instead of being fetched from peers, so a
     * catch-up rebuild costs O(changed pages) of network I/O. In-memory
     * only, guarded by s->lock; cleared on export destroy. */
    uint8_t *meta_blob_cache[EFS_MAX_EXPORTS];
    uint32_t meta_blob_cache_len[EFS_MAX_EXPORTS];
    uint64_t meta_blob_cache_gen[EFS_MAX_EXPORTS];
    uint8_t *meta_blob_sums[EFS_MAX_EXPORTS]; /* pages * 3 * EFS_HASH_SIZE */
    uint32_t meta_blob_pages[EFS_MAX_EXPORTS];

    /* Per-shard incremental rebuild cache: [export slot][shard id]. Same
     * blob+checksums scheme as meta_blob_cache, but keyed by the shard
     * table's own root (the extra-shard descriptor). Without it every
     * extra-shard descriptor refresh re-fetched every page of that shard
     * (the bits>0 multi-write catchup storm). */
    uint8_t *shard_blob_cache[EFS_MAX_EXPORTS][EFS_META_MAX_SHARDS];
    uint32_t shard_blob_cache_len[EFS_MAX_EXPORTS][EFS_META_MAX_SHARDS];
    uint8_t *shard_blob_sums[EFS_MAX_EXPORTS][EFS_META_MAX_SHARDS];
    uint32_t shard_blob_pages[EFS_MAX_EXPORTS][EFS_META_MAX_SHARDS];

    /* Meta flush election (per export slot): the writer that won a
     * META_FLUSH_BEGIN majority. While live (now < expiry), PUT_META roots
     * from any other writer are rejected STALE, so two clients can never
     * interleave page PUTs on the same dual-slot generation. Cleared on
     * commit or after EFS_META_WRITER_EXPIRY_MS. Guarded by s->lock.
     *
     * Fairness: contenders are queued FIFO (meta_writer_q). When the
     * election is free, only the queue head is granted it — a writer that
     * just lost a race cannot be lapped forever by faster resyncers, which
     * previously starved unlucky clients into ever-growing dirty sets.
     * Queue capacity is EFS_META_WRITER_QMAX. */
    uint64_t meta_writer_id[EFS_MAX_EXPORTS];
    uint64_t meta_writer_gen[EFS_MAX_EXPORTS];
    uint64_t meta_writer_expiry[EFS_MAX_EXPORTS];
    /* When the current holder first won the election. A holder re-BEGINing
     * (flush retry) refreshes expiry each time, so without a hold cap a slow
     * or wedged writer can monopolize the FIFO for minutes while contenders
     * starve on BUSY. Re-grants past EFS_META_WRITER_MAX_HOLD_MS yield to the
     * queue head instead. 0 = no holder. Guarded by s->lock. */
    uint64_t meta_writer_since[EFS_MAX_EXPORTS];
    uint64_t meta_writer_q[EFS_MAX_EXPORTS][EFS_META_WRITER_QMAX];
    uint64_t meta_writer_q_ms[EFS_MAX_EXPORTS][EFS_META_WRITER_QMAX];
    uint32_t meta_writer_q_len[EFS_MAX_EXPORTS];

    /* Phase 2a (server-owned metadata): RPC mutation handlers
     * (INODE_CREATE/UNLINK/...) apply to the in-memory table under s->lock
     * and bump rpc_dirty_ops[export_slot]; the meta-flush thread batches and
     * flushes dirty exports via server_flush_fragmented_meta (primary only).
     * rpc_dirty_cv wakes the flush thread early once EFS_META_FLUSH_OPS
     * accumulate. Guarded by s->lock. */
    uint64_t rpc_dirty_ops[EFS_MAX_EXPORTS];
    pthread_cond_t rpc_dirty_cv;
    pthread_t meta_flush_tid;
    int meta_flush_started;
    /* Serializes server_flush_fragmented_meta: the meta-flush thread and a
     * synchronous REPORT_CHUNKS(fs sync) flush both call it, and without a
     * mutex both compute the same new_gen (= root.generation+1) and race to the
     * peers — the loser's root is rejected STALE (gen <= peer's), so the sync
     * fsync sees 0 peer acks and returns EIO even though the data commits on
     * the retry. Holding this for the whole flush makes each compute a fresh
     * gen. */
    pthread_mutex_t meta_flush_mu;

    /* Open-fd refs + cluster flock (guarded by s->lock). */
    struct efs_ino_hold *ino_holds;

    /* Catchup/heal progress for EFS_MSG_HEAL_STATUS (single catchup thread). */
    int heal_active;
    char heal_export[EFS_MAX_NAME];
    uint32_t heal_shard;
    uint32_t heal_pages_done;
    uint32_t heal_pages_total;
    uint64_t heal_gen;
    uint64_t heal_started_us;
    uint64_t heal_last_us;
};

struct efs_msg_heal_status_reply;
void server_fill_heal_status(struct efsd_server *s,
                             struct efs_msg_heal_status_reply *r);

/* A crashed writer's flush election self-clears after this long. Must
 * comfortably exceed the slowest legitimate flush (page PUTs + root),
 * including a starved client's first huge dirty-set flush. */
#define EFS_META_WRITER_EXPIRY_MS 60000ull

/* Max wall-clock time one writer may hold the flush election across re-BEGIN
 * retries while contenders are queued. Generous vs. any legitimate flush
 * (page PUTs + root commit, even a starved client's large dirty set), so a
 * healthy writer never hits it — but a wedged/slow one yields to the FIFO
 * head instead of monopolizing the election for the whole client race
 * budget. Only applies under contention (queue non-empty). */
#define EFS_META_WRITER_MAX_HOLD_MS 30000ull

/* Queued contenders keep their FIFO slot for this long without re-BEGINing.
 * Must exceed the slowest STALE resync (fetch full blob + deserialize a
 * multi-million-row table + rebase a huge dirty set) — a resyncing queue
 * head that lost its slot here was starved forever: every resync finished
 * to find the slot expired and the gen lapped again. A dead head costs one
 * window of stall; live writers re-BEGIN every <1s so false drops need the
 * full window of silence. */
#define EFS_META_WRITER_Q_EXPIRY_MS 300000ull

/* Phase 2a: server-side meta-flush batching. The flush thread commits a
 * dirty export at most every EFS_META_FLUSH_MS, or early once
 * EFS_META_FLUSH_OPS RPC mutations accumulate (whichever first).
 * 100 ms flushed on every create window and fanned extras-commit catchup
 * (9-way unlink-storm create wedged at ~4 files/s). fsync still flushes
 * synchronously. */
#define EFS_META_FLUSH_MS 10000ull
#define EFS_META_FLUSH_OPS 20000ull

/* Global server instance used by worker threads. */
extern struct efsd_server *g_server;

/* Find or create an export by name. */
struct efs_export *server_find_export(struct efsd_server *s, const char *name);
/* Exact-name lookup without create/placeholder-rebrand side effects. */
struct efs_export *server_find_export_no_create(struct efsd_server *s,
                                                const char *name);

/* Get export by id. */
struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id);
/* By-id find-or-create for the replication paths. Caller holds s->lock. */
struct efs_export *server_get_export_create(struct efsd_server *s,
                                            efs_export_id_t id,
                                            const char *name);

/* Export lifetime for unlocked I/O. Acquire returns the export (or NULL if
 * missing/being destroyed) with a use-count held; the caller MUST pair it with
 * server_export_put. Caller holds s->lock on entry to acquire (it does not
 * take it). While the count is held, destroy waits, so the pointer stays
 * valid across unlocked disk I/O. */
struct efs_export *server_export_acquire_locked(struct efsd_server *s,
                                                efs_export_id_t id);
struct efs_export *server_export_acquire(struct efsd_server *s,
                                         efs_export_id_t id);
void server_export_put(struct efsd_server *s, struct efs_export *ex);
/* Index of ex within s->exports, or -1. Caller holds s->lock. */
int server_export_index_locked(struct efsd_server *s, struct efs_export *ex);

/* Destroy an export by name: wipe local data/meta and drop the in-memory row.
 * Returns EFS_OK, EFS_ERR_NOT_FOUND, or EFS_ERR_INVAL. */
int server_destroy_export(struct efsd_server *s, const char *name);

/* Migrate pre-subdirectory storage layout to data/meta/log. */
void server_migrate_old_layout(struct efsd_server *s);

/* Load all exports from storage path. */
void server_load_exports(struct efsd_server *s);

/* Save an export to disk. */
void server_save_export(struct efsd_server *s, struct efs_export *ex);

/* Fragment byte length for this inode: meta pages are fixed; data uses export. */
static inline uint32_t server_frag_len(const struct efs_export *ex, efs_ino_t ino)
{
    if (efs_ino_is_meta_table(ino))
        return EFS_META_FRAGMENT_SIZE;
    uint32_t cs = (ex && efs_chunk_size_valid(ex->chunk_size))
                      ? ex->chunk_size
                      : EFS_DEFAULT_CHUNK_SIZE;
    return efs_frag_size(cs);
}

static inline uint32_t server_data_chunk_size(const struct efs_export *ex)
{
    return (ex && efs_chunk_size_valid(ex->chunk_size))
               ? ex->chunk_size
               : EFS_DEFAULT_CHUNK_SIZE;
}

/* Get the (sharded) path for a fragment on disk. Writes always use this. */
int server_fragment_path(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         char *path, size_t path_len);

/* Unlink fragment + .sum at sharded and legacy flat-{ino} locations. */
void server_unlink_fragment_files(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index);

/* Read a fragment from disk. */
int server_read_fragment(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         uint8_t *data, uint32_t *data_len);

/* Combined fragment+checksum read (single-root fast path). *sum_ok set when
 * the sidecar supplied the checksum; caller hashes the data otherwise. */
int server_read_fragment_with_sum(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index, uint8_t *data,
                                  uint32_t *data_len,
                                  uint8_t checksum[EFS_HASH_SIZE], int *sum_ok);

/* Free per-connection-thread hot-path arenas (call when a conn thread ends). */
void server_handler_tls_cleanup(void);

/* Writer-thread hint from writer.c: payload already verified as all zeros. */
extern __thread int efs_tls_write_known_zero;

/* Writer-thread: storage root index for the in-flight fragment write
 * (-1 = unset; store path helpers fall back to probing / legacy RR). */
extern __thread int efs_tls_write_root;

/* Which local --storage root already holds this fragment, or -1. */
int server_find_fragment_root(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index,
                              uint32_t fragment_index);

/* Synchronous fragment write (disk I/O); used by the writer pool. */
int server_write_fragment_sync(struct efsd_server *s, struct efs_export *ex,
                               efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                               const uint8_t *data, uint32_t data_len);
int server_write_fragment_with_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                        efs_ino_t ino, uint32_t chunk_index,
                                        uint32_t fragment_index,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t checksum[EFS_HASH_SIZE]);

/* Queue fragment write onto the writer pool (or run inline if pool is off). */
int server_write_fragment(struct efsd_server *s, struct efs_export *ex,
                          efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                          const uint8_t *data, uint32_t data_len);

/* Fragment data + checksum sidecar as one pooled job. */
int server_write_fragment_with_sum(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t *data, uint32_t data_len,
                                   const uint8_t checksum[EFS_HASH_SIZE]);

int server_default_writer_threads(void);
int server_writer_pool_start(struct efsd_server *s);
void server_writer_pool_stop(struct efsd_server *s);
/* Grow the least-q path set after a live add-storage (append-only). */
void server_writer_set_npaths(uint32_t n);

/* Append local storage roots without restart. csv is comma-separated
 * absolute paths. Existing roots are skipped. count_out is the new total. */
int server_add_storage_paths(struct efsd_server *s, const char *csv,
                             uint32_t *count_out);

/* Async Blake3 of a stored fragment vs the client sidecar; heal from peers. */
int server_verify_start(struct efsd_server *s);
void server_verify_stop(struct efsd_server *s);
void server_verify_enqueue(struct efsd_server *s, efs_export_id_t export_id,
                           efs_ino_t ino, uint32_t chunk_index,
                           uint32_t fragment_index, uint32_t data_len,
                           const uint8_t checksum[EFS_HASH_SIZE]);

/* pthread_create with a larger stack (hello_ack / node snapshots are ~16KiB). */
int efsd_pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg);

/* Compute total bytes used under one storage root's data/. */
uint64_t server_compute_usage(const char *path);

/* Bytes used across all local data roots (sum of on-disk sizes). */
uint64_t server_compute_local_usage(struct efsd_server *s);

/* Update the local node's used counter via a full data/ tree scan, then persist. */
void server_update_local_usage(struct efsd_server *s);

/* Load meta/usage.bin, or scan+save if missing/corrupt. Call once at startup. */
void server_init_local_usage(struct efsd_server *s);

/* Persist local->used to meta/usage.bin (primary storage root). */
void server_usage_save(struct efsd_server *s);

/* Mark usage dirty (hot path). Heartbeat / explicit save flushes. */
void server_usage_mark_dirty(struct efsd_server *s);

/* If usage_dirty, persist and clear the flag. */
void server_usage_flush_dirty(struct efsd_server *s);

/* Comma-join all local storage roots into buf for status/HELLO advertise. */
void server_format_storage_paths(const struct efsd_server *s, char *buf, size_t buflen);

/* Pointer to this process's row in s->nodes (matched by s->id), or NULL. */
struct efs_node *server_local_node(struct efsd_server *s);

/* Update (or append) this process's row from s->addr/port/quota/storage.
 * Deduplicates by node id. Caller may hold s->lock or not (takes lock). */
void server_sync_local_membership(struct efsd_server *s);

/* Compact s->nodes to unique ids (first wins). Caller must hold s->lock. */
void server_dedupe_nodes_locked(struct efsd_server *s);

/* Return true if adding fragment_size bytes would exceed the server's quota. */
bool server_would_exceed_quota(struct efsd_server *s, uint64_t fragment_size);

/* Handle one client connection. */
struct efs_conn;
void server_handle_conn(struct efs_conn *conn);

/* Join an existing cluster by contacting a peer. */
int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port);

/* Relay a membership change to all other peers (ring convergence). */
struct efs_msg_hello;
void server_gossip_membership(struct efsd_server *s, const struct efs_msg_hello *h);

/* Persist export as 2+1 meta pages + EFSR root; push root to peers. */
int server_flush_fragmented_meta(struct efsd_server *s, struct efs_export *ex);

/* Best-effort unlink local meta page fragments for a retired generation slot.
 * Non-fatal; space leak only if unlink fails. With dirty-page flushing the
 * live generation may still reference (skip re-PUTting) unchanged pages whose
 * fragments were written by an earlier same-parity generation, so only pages
 * BEYOND the new live generation's page count are truly dead and unlinked;
 * in-range fragments are left in place for reuse (they are overwritten by
 * the next same-parity flush that actually changes them). */
void server_gc_meta_slot_pages(struct efsd_server *s, struct efs_export *ex,
                               uint64_t dead_generation,
                               uint32_t old_ino_pages, uint32_t old_chunk_pages,
                               uint32_t live_ino_pages, uint32_t live_chunk_pages);
/* CoW (EFSR v7) GC: reclaim cis in old_cis[] no longer referenced by
 * new_cis[] (the new committed root's page_cis[]). Both arrays are caller-
 * owned copies captured under the server lock (the GC runs lock-free). */
void server_gc_meta_cow_pages(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t table_ino,
                              const uint32_t *old_cis, uint32_t old_count,
                              const uint32_t *new_cis, uint32_t new_count);

/* Rebuild in-memory export tables from meta pages referenced by ex->root. */
int server_rebuild_export_from_pages(struct efsd_server *s, struct efs_export *ex);

/* Caller holds s->lock. If this node owns `shard` and the table is still
 * hollow (descriptor present, pages not assembled), drop the lock, rebuild
 * from pages, and reacquire. Returns 0 when the table is safe to mutate,
 * -1 if it is still hollow (caller should reply BUSY). */
int server_ensure_shard_ready(struct efsd_server *s, struct efs_export *ex,
                              uint32_t shard);

/* After membership is known, rebuild any EFSR exports from meta pages. */
void server_rebuild_fragmented_exports(struct efsd_server *s);

/* Send metadata to all peers. Returns number of acks. */
int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex);

/* Send a metadata snapshot (EFSR root or legacy EFSM) to a peer. */
int server_send_metadata_to(struct efsd_server *s, struct efs_export *ex,
                              const char *host, uint16_t port);

/* Fetch metadata from a peer. */
int server_fetch_metadata_from(struct efsd_server *s, const char *host, uint16_t port);

/* Start the background heartbeat thread. */
void server_start_heartbeat(struct efsd_server *s);

/* Non-zero when a node is heartbeat-marked-down (exclude from placement).
 * Caller holds s->lock. */
int server_node_is_down_locked(struct efsd_server *s, efs_node_id_t id);

/* Start the background data migration thread. */
void server_start_migration(struct efsd_server *s);

/* Start background meta catch-up (rebuild dirty roots + heal local pages). */
void server_start_meta_catchup(struct efsd_server *s);

/* Phase 2a: non-zero when this server is the metadata primary (lowest-id
 * live node) — the single writer that flushes RPC-driven dirty exports.
 * Caller holds s->lock. */
int server_is_meta_primary_locked(struct efsd_server *s);

/* Phase 2b: the metadata primary's node id (lowest-id live node). Caller
 * holds s->lock. */
efs_node_id_t server_meta_primary_id_locked(struct efsd_server *s);

/* Phase 2a: start the background meta-flush thread (batched server-side
 * flush of RPC-driven dirty exports). */
void server_start_meta_flush(struct efsd_server *s);

/* Phase 2a: note an RPC mutation on export slot `eidx` (bumps the dirty-ops
 * counter; signals the flush thread once EFS_META_FLUSH_OPS accumulate).
 * Caller holds s->lock. */
void server_meta_mark_rpc_dirty_locked(struct efsd_server *s, uint32_t eidx);

/* Start the background cluster rejoin retry thread. */
void server_start_rejoin(struct efsd_server *s);

/* Remove a node from the local cluster list. */
void server_remove_node_from_cluster(struct efsd_server *s, efs_node_id_t node_id);

/* Broadcast a node-left notification to all peers. */
void server_notify_node_left(struct efsd_server *s, efs_node_id_t node_id);

/* Re-place fragments orphaned by a departed node and flush the EFSR. */
void server_heal_orphan_fragments(struct efsd_server *s, efs_node_id_t node_id);
int server_rebuild_orphan_fragment(struct efsd_server *s, struct efs_export *ex,
                                   struct efs_chunk_entry *chunk,
                                   int fragment_index, efs_node_id_t dead_id);

/* True if any chunk still places a fragment on this node's id. Caller holds lock. */
int server_has_local_fragments_locked(struct efsd_server *s);

/* Rebuild meta maps + refresh used; return 1 if no local fragments and used==0. */
int server_node_is_empty(struct efsd_server *s);

/* Begin leave: notify peers and stop accepting connections. Caller holds lock. */
void server_begin_leave_locked(struct efsd_server *s);

/* Persist / load a fragment's blake3 checksum sidecar (written once at PUT). */
int server_write_fragment_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t checksum[EFS_HASH_SIZE]);
int server_write_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                              const uint8_t checksum[EFS_HASH_SIZE]);
int server_read_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                             efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                             uint8_t checksum[EFS_HASH_SIZE]);

/* Persist the cluster nodes list to disk. Caller must NOT hold s->lock (does
 * disk I/O). Use server_nodes_mark_dirty under the lock instead. */
void server_save_nodes(struct efsd_server *s);

/* Mark membership dirty (under s->lock); flush with server_nodes_flush_dirty
 * after dropping the lock so the fsync never stalls the global lock. */
void server_nodes_mark_dirty(struct efsd_server *s);
void server_nodes_flush_dirty(struct efsd_server *s);

/* Load the persisted cluster nodes list from disk. */
void server_load_nodes(struct efsd_server *s);

/* Try to rejoin the cluster using any persisted peer. Returns 0 on success. */
int server_rejoin_cluster(struct efsd_server *s);

#endif
