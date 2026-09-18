#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <pthread.h>
#include <time.h>

/* Soft cap on concurrent accept/handler threads. Excess sockets are closed
 * immediately so one connection storm cannot exhaust RLIMIT_NOFILE. */
#define EFS_SERVER_MAX_CONNS 4096

/* Persistent server→server pool (meta/migrate/heartbeat/status). TCP plus
 * RDMA upgrade when a usable IB HCA exists. */
void server_peer_pool_init(void);
void server_peer_pool_shutdown(void);
/* Checkout a live conn to host:port (connects on miss). Returns NULL on failure. */
struct efs_conn *server_peer_conn_get(const char *host, uint16_t port);
/* Return conn to the pool after a successful request/response. */
void server_peer_conn_release(const char *host, uint16_t port, struct efs_conn *c);
/* Close and discard a broken conn (net/protocol error). */
void server_peer_conn_drop(const char *host, uint16_t port, struct efs_conn *c);
/* Shared PUT writer pool (one queue, all --storage paths). --writers n is
 * the total; n=0 is inline; n<0 (startup default) means auto from nproc. */
#define EFS_WRITERS_RESERVED         4  /* main + heartbeat + migrate + catchup */
#define EFS_MAX_WRITERS              64
#define EFS_MAX_WRITERS_PER_PATH     EFS_MAX_WRITERS /* CLI / bench alias */
#define EFS_DEFAULT_WRITERS          8               /* bench fallback */
#define EFS_WRITERS_PER_PATH_DEFAULT EFS_DEFAULT_WRITERS

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
     * an export slot cannot be freed/compacted out from under them.
     * Guarded by s->lock; a teardown sets destroying then waits on
     * export_idle_cv until export_inflight drains to 0. */
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

    pthread_t rejoin_tid; /* background rejoin retry thread */
    char rejoin_addr[64]; /* explicit --join target to keep retrying; empty = use persisted peers */
    uint16_t rejoin_port;

    int direct_io; /* use O_DIRECT for fragment reads/writes */
    /* Shared writer-pool size (0 = inline, <0 = auto from nproc at start). */
    int nwriters;
    int persist_nodes; /* persist cluster membership to disk */
    int perf; /* run under perf record when starting */
    int usage_dirty; /* local->used changed; flush meta/usage.bin soon */
    int nodes_dirty; /* membership changed; persist nodes.bin off the lock */

    /* Open-fd refs + cluster flock (guarded by s->lock). */
    struct efs_ino_hold *ino_holds;

};


/* Global server instance used by worker threads. */
extern struct efsd_server *g_server;

/* Get export by id. */
struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id);
/* Export lifetime for unlocked I/O. Acquire returns the export (or NULL if
 * missing/being destroyed) with a use-count held; the caller MUST pair it with
 * server_export_put. Caller holds s->lock on entry to acquire (it does not
 * take it). While the count is held, destroy waits, so the pointer stays
 * valid across unlocked disk I/O. */
struct efs_export *server_export_acquire_locked(struct efsd_server *s,
                                                efs_export_id_t id);
void server_export_put(struct efsd_server *s, struct efs_export *ex);
/* Index of ex within s->exports, or -1. Caller holds s->lock. */
int server_export_index_locked(struct efsd_server *s, struct efs_export *ex);

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

/* Unlink a fragment's data + .sum sidecar from every storage root. */
void server_unlink_fragment_files(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index);

/* Checksum-conditional fragment delete for the data-plane GC (L7).
 * EFS_OK = dead fragment gone; EFS_ERR_EXIST = a mismatched live fragment
 * occupies the slot (ackable); EFS_ERR_IO = real failure (retry). */
int server_delete_fragment_if_sum(struct efsd_server *s, struct efs_export *ex,
                                  efs_ino_t ino, uint32_t chunk_index,
                                  uint32_t fragment_index,
                                  const uint8_t expect_sum[EFS_HASH_SIZE]);

/* NVMe adapter for efs/store.h. Bind to an already-acquired export. */
struct efs_nvme_store {
    struct efsd_server *s;
    struct efs_export *ex;
};
struct efs_store;
void efs_store_nvme_bind(struct efs_store *st, struct efs_nvme_store *ctx,
                         struct efsd_server *s, struct efs_export *ex);

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
 * (-1 = unset; store path helpers fall back to probing / round-robin). */
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
/* Handle one client connection. */
struct efs_conn;
void server_handle_conn(struct efs_conn *conn);

/* Join an existing cluster by contacting a peer. */
int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port);

/* Relay a membership change to all other peers (ring convergence). */
struct efs_msg_hello;
void server_gossip_membership(struct efsd_server *s, const struct efs_msg_hello *h);

/* Start the background heartbeat thread. */
void server_start_heartbeat(struct efsd_server *s);


/* Production Raft host (architecture.md §10 10.5c). Starts unconditionally
 * (the old 2PC engine is gone). Start fails loud on setup error. Stop is
 * safe if never started. */
int server_raft_host_start(struct efsd_server *s);
void server_raft_host_stop(void);
/* Copy one encoded efs_raft_msg into the pump inbox. Delivery ack is the
 * caller's job (EFS_MSG_RAFT_REPLY). Drops when the host is off or the
 * inbox is full — Raft retries. */
int server_raft_host_inbox(const uint8_t *payload, uint32_t plen);
void server_raft_host_mkfs(struct efs_msg_raft_mkfs_reply *out);
/* Non-empty RAFT_MKFS payload: group byte + command (or group-only =
 * ReadIndex). Leader-only; NOT_PRIMARY otherwise. */
void server_raft_host_submit(const uint8_t *payload, uint32_t plen,
                             struct efs_msg_raft_mkfs_reply *out);
void server_raft_host_status(struct efs_msg_raft_status_reply *out);
int server_raft_host_active(void);
/* Leader + ReadIndex + applied KV. No s->lock. Inactive before mkfs
 * and these are never called. NOT_PRIMARY fills primary_id = raft leader
 * node id. */
void server_raft_host_getattr(efs_ino_t ino, struct efs_msg_inode_reply *out);
void server_raft_host_hold(efs_ino_t ino, uint32_t flags, uint64_t owner,
                           const uint8_t *sess_uuid, uint32_t sess_epoch,
                           struct efs_msg_inode_reply *out);
void server_raft_host_flock(efs_ino_t ino, uint32_t op, uint64_t owner,
                            uint64_t start, uint64_t end,
                            const uint8_t *sess_uuid, uint32_t sess_epoch,
                            struct efs_msg_inode_reply *out);
void server_raft_host_lookup(efs_ino_t parent, const char *name,
                             struct efs_msg_inode_reply *out);
void server_raft_host_create(efs_ino_t parent, const char *name, uint32_t mode,
                             uint32_t uid, uint32_t gid, uint32_t flags,
                             uint64_t owner, struct efs_msg_inode_reply *out);
void server_raft_host_mkdir(efs_ino_t parent, const char *name, uint32_t mode,
                            uint32_t uid, uint32_t gid,
                            struct efs_msg_inode_reply *out);
void server_raft_host_unlink(efs_ino_t parent, const char *name, int is_dir,
                             struct efs_msg_inode_reply *out);
void server_raft_host_rmdir(efs_ino_t parent, const char *name,
                            struct efs_msg_inode_reply *out);
void server_raft_host_setattr(efs_ino_t ino, uint32_t mask, uint32_t mode,
                              uint32_t uid, uint32_t gid, uint64_t size,
                              uint64_t mtime, uint32_t mtime_nsec,
                              uint64_t atime, struct efs_msg_inode_reply *out);
void server_raft_host_append(efs_ino_t ino, uint64_t len,
                             const uint8_t *sess_uuid, uint32_t sess_epoch,
                             uint64_t op_seq, struct efs_msg_inode_reply *out);
void server_raft_host_link(efs_ino_t src_ino, efs_ino_t new_parent,
                           const char *new_name, struct efs_msg_inode_reply *out);
void server_raft_host_rename_at(efs_ino_t old_parent, const char *old_name,
                                efs_ino_t new_parent, const char *new_name,
                                struct efs_msg_inode_reply *out);
void server_raft_host_readdir(efs_ino_t parent, uint32_t max_ents,
                              uint32_t after_src, const char *after_name,
                              struct efs_msg_inode_readdir_reply *out);
void server_raft_host_lookup_path(efs_ino_t start, const char *path,
                                  struct efs_msg_inode_lookup_path_reply *out);
void server_raft_host_report(const struct efs_chunk_rec *recs, uint32_t count,
                             const struct efs_ino_size_rec *irecs,
                             uint32_t ino_count, struct efs_msg_inode_reply *out);
void server_raft_host_getchunks(efs_ino_t ino, uint32_t start, uint32_t max,
                                struct efs_msg_inode_getchunks_reply *out);

/* Start the background cluster rejoin retry thread. */
void server_start_rejoin(struct efsd_server *s);

/* Remove a node from the local cluster list. */
void server_remove_node_from_cluster(struct efsd_server *s, efs_node_id_t node_id);

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
