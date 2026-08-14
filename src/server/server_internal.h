#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <pthread.h>

/* Soft cap on concurrent accept/handler threads. Excess sockets are closed
 * immediately so one connection storm cannot exhaust RLIMIT_NOFILE. */
#define EFS_SERVER_MAX_CONNS 512

/* Persistent server→server TCP pool (meta/migrate/heartbeat/status). */
void server_peer_pool_init(void);
void server_peer_pool_shutdown(void);
/* Checkout a live fd to host:port (connects on miss). Returns -1 on failure. */
int server_peer_conn_get(const char *host, uint16_t port);
/* Return fd to the pool after a successful request/response. */
void server_peer_conn_release(const char *host, uint16_t port, int fd);
/* Close and discard a broken fd (net/protocol error). */
void server_peer_conn_drop(const char *host, uint16_t port, int fd);
/* Dedicated PUT writers per --storage path (stripe lane). */
#define EFS_WRITERS_PER_PATH_DEFAULT 8
#define EFS_MAX_WRITERS_PER_PATH     64
/* Back-compat aliases used by local --bench. */
#define EFS_DEFAULT_WRITERS  EFS_WRITERS_PER_PATH_DEFAULT
#define EFS_MAX_WRITERS      EFS_MAX_WRITERS_PER_PATH

enum efsd_server_state {
    SERVER_STATE_ACTIVE = 0,
    SERVER_STATE_LEAVING = 1,   /* leave cluster after successful remove (no migrate) */
    SERVER_STATE_SHRINKING = 2, /* reducing local quota usage */
    SERVER_STATE_DRAINING = 3,  /* migrating local fragments to peers */
    SERVER_STATE_DRAINED = 4,   /* empty; rejects new PUTs until undrain or remove */
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
    /* Writers per storage path (0 = inline). Total = nwriters * path_count. */
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
};

/* Global server instance used by worker threads. */
extern struct efsd_server *g_server;

/* Find or create an export by name. */
struct efs_export *server_find_export(struct efsd_server *s, const char *name);

/* Get export by id. */
struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id);

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
    if (ino == EFS_META_TABLE_INO)
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

int server_writer_pool_start(struct efsd_server *s);
void server_writer_pool_stop(struct efsd_server *s);

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
void server_handle_conn(int fd);

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
                               uint64_t dead_generation, uint32_t page_count,
                               uint32_t live_page_count);

/* Rebuild in-memory export tables from meta pages referenced by ex->root. */
int server_rebuild_export_from_pages(struct efsd_server *s, struct efs_export *ex);

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
