#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/query_catalog.h"
#include "efs/raft_frame.h"
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
/* Heartbeat success: end host:port's connect cooldown (peer_pool.c). */
void server_peer_note_up(const char *host, uint16_t port);
struct efs_conn *server_peer_read_conn_get(const char *host, uint16_t port);
/* Unpooled connection owned by the caller (destroy with efs_conn_destroy). */
struct efs_conn *server_peer_conn_new(const char *host, uint16_t port);
/* Return conn to the pool after a successful request/response. */
void server_peer_conn_release(const char *host, uint16_t port, struct efs_conn *c);
/* Close and discard a broken conn (net/protocol error). */
void server_peer_conn_drop(const char *host, uint16_t port, struct efs_conn *c);
/* Shared PUT writer pool (one queue, all --storage paths). --writers n is
 * the total; n=0 is inline; n<0 requests CPU-based sizing for internal callers.
 * Production startup defaults to EFS_DEFAULT_WRITERS. */
#define EFS_WRITERS_RESERVED         4  /* main + heartbeat + migrate + catchup */
#define EFS_MAX_WRITERS              64
#define EFS_MAX_WRITERS_PER_PATH     EFS_MAX_WRITERS /* CLI / bench alias */
#define EFS_DEFAULT_WRITERS          8               /* daemon default / bench fallback */
#define EFS_WRITERS_PER_PATH_DEFAULT EFS_DEFAULT_WRITERS

struct efsd_server {
    uint64_t put_boot_epoch; /* durability verifier, stable across proven clean restarts */
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
    /* D6. Empty means mdraft lives under storage_path (the first
     * --storage root). --meta-storage names a different root; nothing
     * moves an existing mdraft on its own. */
    char meta_storage[EFS_MAX_PATH];
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

    /* Runtime GC progress: atomically updated, reset on daemon restart. */
    uint64_t gc_removed_fragments, gc_removed_file_bytes, gc_removed_payload_bytes;
    uint64_t gc_delete_errors, gc_mismatches, gc_last_delete_us;
    uint64_t gc_pass_start_us, gc_last_pass_us, gc_passes;
    uint64_t gc_sweep_errors, gc_reap_errors, gc_ack_errors, gc_scan_errors;
    uint64_t gc_trunc_seen[2]; /* D25 TRUNC_PENDING inodes at the last pass */
    uint64_t gc_reap_seen[2], gc_orphan_seen[2], gc_pending[2], gc_first_reap[2], gc_first_seen_us[2];
    uint64_t gc_missing_exports;
    /* W124 node reclaim (reclaim.c). Bytes credited back to quota at
     * acceptance but still on disk; physical admission guard state. */
    uint64_t reclaim_pending_bytes;
    int reclaim_fail_closed; /* journal loss detected: refuse data PUTs */
    /* Per filesystem: usable bytes at the last sample, bytes admitted since.
     * phys_fs[r] maps storage root r to its filesystem slot + 1 (0 = not
     * sampled yet: admitted). */
    uint64_t phys_avail[EFS_MAX_STORAGE_PATHS], phys_charged[EFS_MAX_STORAGE_PATHS];
    uint8_t phys_fs[EFS_MAX_STORAGE_PATHS];
    uint64_t phys_refused;
    struct efs_gc_io_stats gc_io[EFS_GC_IO_CLASSES];
    uint32_t gc_stage, gc_group, gc_sampled_mask, gc_reap_capped[2];
    void *storage_roots[EFS_MAX_STORAGE_PATHS]; /* root identity/locks (storage_roots.c) */
    int direct_io; /* use O_DIRECT for fragment reads/writes */
    int bench_sync; /* benchmark-only O_SYNC; daemon store builds ignore it */
    /* Shared writer-pool size (0 = inline, <0 = auto from nproc at start). */
    int nwriters;
    int persist_nodes; /* persist cluster membership to disk */
    int perf; /* run under perf record when starting */
    int strace; /* attach strace -f (EFS_STRACE_PATH / EFS_STRACE_EXPR) */
    int usage_dirty; /* local->used changed; flush meta/usage.bin soon */
    int nodes_dirty; /* membership changed; persist nodes.bin off the lock */

};


/* Global server instance used by worker threads. */
extern struct efsd_server *g_server;
/* Local storage barrier only, never a session or ticket admission permit.
 * A future ticketed receiver must obtain fresh authoritative ADMITTED proof,
 * then hold this guard through the complete disk write. FENCE_LOC waits for
 * old guarded writes before sealing reject_below, and cannot ACK a failed seal.
 * UUID floors are retained until a durable admission-retirement policy exists. */
struct efs_put_storage_guard { void *locked; };
int server_put_session_begin(struct efsd_server *, const uint8_t uuid[16],
                             uint32_t epoch, struct efs_put_storage_guard *);
void server_put_session_end(struct efs_put_storage_guard *);
void server_raft_host_storage_session_fence(const struct efs_msg_storage_session_fence *,
    struct efs_msg_storage_session_fence_reply *);
int server_put_upload(struct efsd_server *,const uint8_t *,uint32_t,uint8_t *);
int server_put_session_fence(struct efsd_server *, const uint8_t uuid[16],
                             uint32_t reject_below);



/* Get export by id. */
struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id);
/* Export lifetime for unlocked I/O. Acquire returns the export (or NULL if
 * missing/being destroyed) with a use-count held; the caller MUST pair it with
 * server_export_put. Caller holds s->lock on entry to acquire (it does not
 * take it). While the count is held, destroy waits, so the pointer stays
 * valid across unlocked disk I/O. */
struct efs_export *server_export_acquire_locked(struct efsd_server *s,
                                                efs_export_id_t id);
/* GET after a server restart must not wait for a PUT to mint the shell —
 * fragments already live under data/exports/<id>/ (W1 remount decode). */
struct efs_export *server_export_acquire_or_create_locked(struct efsd_server *s,
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

/* Get the (sharded) path for a fragment on disk on the picked write root
 * (efs_tls_write_root). Writes always use this; EFS_ERR_INVAL without one. */
int server_fragment_path(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         char *path, size_t path_len);

/* Unlink a fragment's data + .sum sidecar from every storage root. */
int server_unlink_fragment_files(struct efsd_server *s, struct efs_export *ex,
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

/* Combined fragment+checksum read (single-root fast path). *sum_ok uses
 * efs_store_sum_state, nonzero when a stored checksum exists. PRESENT
 * requires verification; NONE is untrusted for data fragments. */
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

/* W14.4. path_hint is the client's guess (-1 = none, -2 = first write
 * of this generation: do not access(), create on the least-queued root).
 * path_used is the root the PUT landed on (-1 until the write picks one). */
#define EFS_PATH_HINT_SKIP (-2)
extern __thread int efs_tls_path_hint;
extern __thread int efs_tls_path_used;

/* Candidate generation for the in-flight fragment PUT/GET (0 = legacy
 * `{ci}.{fi}` path). Copied onto the writer job so the pool thread
 * builds the same name the handler intended. */
extern __thread uint64_t efs_tls_chunk_gen;

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
/* Opt-in diagnostics; enable/reset only while no writes are in flight. */
struct efs_writer_stats {
    uint64_t jobs, queued, fallback, active, peak_active;
    uint64_t admission_us, admission_max_us, queue_us, queue_max_us;
    uint64_t service_us, service_max_us, resume_us, resume_max_us;
    uint64_t lock_us, lock_max_us;
};
void server_writer_stats_reset(int enabled);
void server_writer_stats_snapshot(struct efs_writer_stats *out);
int server_writer_pool_start(struct efsd_server *s);
void server_writer_pool_stop(struct efsd_server *s);
/* Grow the least-q path set after a live add-storage (append-only). */
void server_writer_set_npaths(uint32_t n);

/* Always-on per-op-class I/O stats (iostats.c): relaxed atomics, two
 * clock_gettime(CLOCK_MONOTONIC) calls per op, no locks or allocation.
 * Class ids index efs_msg_io_stats_reply.cls. */
#define EFS_IOSTAT_GET        0 /* GET_CHUNK handler, conn thread */
#define EFS_IOSTAT_PUT        1 /* PUT_CHUNK handler, conn thread */
#define EFS_IOSTAT_DISK_WRITE 2 /* writer-pool disk write */
#define EFS_IOSTAT_DISK_READ  3 /* filesystem-backed fragment read */
#define EFS_IOSTAT_GC_DELETE  4 /* GC_INODE_BATCH RPC */
uint64_t efs_iostats_now_us(void);
void efs_iostats_add(int cls, uint64_t bytes, uint64_t us, int err);
void efs_iostats_snapshot(struct efs_msg_io_stats_reply *out);
/* 5 s throttled `iostats:` stderr line once any class is nonzero; the
 * raft host pump calls it on the host_obs_dump cadence. */
void efs_iostats_dump(int force);

/* Append local storage roots without restart. csv is comma-separated
 * absolute paths. Existing roots are skipped. count_out is the new total. */
void server_gc_status(struct efsd_server *s, struct efs_msg_gc_status_reply *out);

int server_add_storage_paths(struct efsd_server *s, const char *csv,
                             uint32_t *count_out);

/* pthread_create with a larger stack (hello_ack / node snapshots are ~16KiB). */
int efsd_pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg);

/* Compute total bytes used under one storage root's data/. */
uint64_t server_compute_usage(const char *path);
/* Exact per-root inventory, joined and fail-closed; at most four file walks. */
int server_compute_roots_usage(struct efsd_server *s, uint64_t *usage);

/* Bytes used across all local data roots (sum of on-disk sizes). */
uint64_t server_compute_local_usage(struct efsd_server *s);

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
 * caller's job (EFS_MSG_RAFT_REPLY). BUSY accepts no frame; Raft retries. */
int server_raft_host_inbox(const uint8_t *payload, uint32_t plen);
void server_raft_host_mkfs(struct efs_msg_raft_mkfs_reply *out);
/* Non-empty RAFT_MKFS payload: group byte + command (or group-only =
 * ReadIndex). Leader-only; NOT_PRIMARY otherwise. */
void server_raft_host_submit(const uint8_t *payload, uint32_t plen,
                             struct efs_msg_raft_mkfs_reply *out);
void server_raft_host_expiry_status(struct efs_expiry_status *out);
void server_raft_host_status(struct efs_msg_raft_status_reply *out);
void server_raft_host_resync(const struct efs_msg_raft_resync *in, struct efs_msg_raft_resync_reply *out);
int server_raft_host_active(void);
int server_raft_host_inode_reaped(efs_ino_t ino, int *dead);
/* Borrowed immutable query image; group slots are 0 and 2. Only background
 * refresh builds catalogs. The callback must enforce fresh authorization and
 * never retain pointers. Failed refresh preserves source/build watermarks. */
struct efs_server_query_view {
    struct efs_query_catalog *catalog;
    uint64_t boot_id,source_ns,built_ns,generation,applied[2],terms[2],pin_ms,build_ms;
    uint64_t pending_changes,pending_since_ms,lag_sample_ms,lag_sample_ns;
};
int server_raft_host_query_features_checked(uint32_t *mask);
int server_raft_host_query_read(int (*consume)(void *,const struct efs_server_query_view *,int),
                                 void *ctx,uint16_t *primary_id);

/* Leader + ReadIndex + applied KV. No s->lock. Inactive before mkfs
 * and these are never called. NOT_PRIMARY fills primary_id = raft leader
 * node id. */
int server_raft_host_query_snapshot(const struct efs_msg_query_snapshot *req, void **reply, uint32_t *len);
int server_raft_host_query_page(const struct efs_msg_query_snapshot_v4 *req,void **reply,uint32_t *len);
void server_raft_host_query_reply_free(void *reply);
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
/* Directory mutations take the request's op-id (`q`, NULL = none; I16):
 * a retry with the same identity gets the recorded verdict. */
struct efs_opid_req;
void server_raft_host_create(efs_ino_t parent, const char *name, uint32_t mode,
                             uint32_t uid, uint32_t gid, uint32_t flags,
                             uint64_t owner, const struct efs_opid_req *q,
                             struct efs_msg_inode_reply *out);
void server_raft_host_create_retained(const struct efs_msg_create_retained *,
                                      struct efs_msg_create_retained_reply *);
void server_raft_host_mkdir(efs_ino_t parent, const char *name, uint32_t mode,
                            uint32_t uid, uint32_t gid,
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out);
void server_raft_host_unlink(efs_ino_t parent, const char *name, int is_dir,
                             const struct efs_opid_req *q,
                             struct efs_msg_inode_reply *out);
void server_raft_host_rmdir(efs_ino_t parent, const char *name,
                            const struct efs_opid_req *q,
                            struct efs_msg_inode_reply *out);
void server_raft_host_setattr(efs_ino_t ino, uint32_t mask, uint32_t mode,
                              uint32_t uid, uint32_t gid, uint64_t size,
                              uint64_t mtime, uint32_t mtime_nsec,
                              uint64_t atime, uint32_t atime_nsec,
                              const struct efs_opid_req *q,
                              struct efs_msg_inode_reply *out);
struct efs_expiry_due;
/* Staged conditional transaction. Not connected to RPC or a live worker. */
void server_raft_host_expire(efs_ino_t parent,const char *name,
                             uint64_t committed_now,
                             const struct efs_expiry_due *,
                             struct efs_msg_inode_reply *);
void server_raft_host_xattr(efs_ino_t ino, uint8_t op, uint32_t flags,
                            const uint8_t *name, uint16_t nlen,
                            const uint8_t *val, uint32_t vlen,
                            struct efs_msg_xattr_reply *out, uint32_t *out_len);
void server_raft_host_append(efs_ino_t ino, uint64_t len,
                             const uint8_t *sess_uuid, uint32_t sess_epoch,
                             uint64_t op_seq, const uint64_t *ack, struct efs_msg_inode_reply *out);
void server_raft_host_link(efs_ino_t src_ino, efs_ino_t new_parent,
                           const char *new_name, const struct efs_opid_req *q,
                           struct efs_msg_inode_reply *out);
void server_raft_host_rename_at(efs_ino_t old_parent, const char *old_name,
                                efs_ino_t new_parent, const char *new_name,
                                const struct efs_opid_req *q,
                                struct efs_msg_inode_reply *out);
void server_raft_host_readdir(efs_ino_t parent, uint32_t max_ents,
                              uint32_t after_src, const char *after_name,
                              struct efs_msg_inode_readdir_reply *out);
void server_raft_host_lookup_path(efs_ino_t start, const char *path,
                                  struct efs_msg_inode_lookup_path_reply *out);
void server_raft_host_report(const struct efs_chunk_rec *recs, uint32_t count,
                             const struct efs_ino_size_rec *irecs,
                             uint32_t ino_count, uint32_t flags, const struct efs_opid *session,
                             struct efs_msg_inode_reply *out);
void server_raft_host_lane_bootstrap_rpc(const struct efs_msg_lane_bootstrap *,
                                          struct efs_msg_lane_writer_view_reply *);
void server_raft_host_publication(const struct efs_msg_publication *, int query_only, /* 0 submit, 1 status, 2 exact retirement */
                                  struct efs_msg_publication_reply *);
void server_raft_host_lane_writer_view(const struct efs_msg_lane_writer_view *,
                                        struct efs_msg_lane_writer_view_reply *);
/* Internal cold coordinator; not enabled in application write admission yet. */
int server_raft_host_lane_bootstrap(efs_ino_t ino, uint64_t generation,
                                     uint32_t ci, uint32_t chunk_size, int *hint);
void server_raft_host_writer_view(const struct efs_msg_inode_writer_view *req,
                                   struct efs_msg_inode_writer_view_reply *out);
/* Internal only: zero on arrival; never retain across inbound requests. */
void server_raft_host_writer_view_reuse(const struct efs_msg_inode_writer_view *req,
                                   struct efs_msg_inode_writer_view_reply *out,
                                   uint8_t *covered);
void server_raft_host_getchunks(efs_export_id_t export_id, efs_ino_t ino, uint32_t start, uint32_t max, uint64_t generation,
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

/* Asynchronous PUT acknowledgement and durability barriers (store.c). */
struct efs_msg_sync_stats_reply;
int server_put_sync_init(struct efsd_server *s);
int server_put_sync_commit(struct efsd_server *s, uint32_t *roots);
void server_put_sync_stats(struct efsd_server *s, struct efs_msg_sync_stats_reply *out);
void server_put_sync_stop(struct efsd_server *s);
/* Mark the start of serving; before it, an exit restores each untouched
 * root's previous durability record (server_put_sync_abandon, atexit). */
void server_put_sync_serving(void);
void server_put_sync_abandon(void);


/* ---- W124 node reclaim (src/server/reclaim.c) ----
 * Batched, journal-first acceptance of dead inodes for physical reclaim. */
#define EFS_RECLAIM_INODE 0u
#define EFS_RECLAIM_SEGMENT 1u /* W125: a retired packed segment's fragments */
#define EFS_RECLAIM_SUPERSEDED 2u /* one superseded fragment of a live inode */
/* A segment's fence lives in the same set as inode fences, under an
 * export id no export uses. */
#define EFS_RECLAIM_SEGMENT_EID(group) (0x80000000u | (uint32_t)(group))
struct efs_reclaim_item {
    uint32_t export_id;  /* SEGMENT: EFS_RECLAIM_SEGMENT_EID(group) */
    efs_ino_t ino;       /* SEGMENT: segment id */
    uint64_t generation; /* 0 = derived by recovery; SEGMENT: segment generation;
                          * SUPERSEDED: the dead chunk generation */
    uint8_t kind;
    uint8_t fragment;     /* SUPERSEDED */
    uint32_t chunk;       /* SUPERSEDED */
    uint8_t sum[EFS_HASH_SIZE]; /* SUPERSEDED: deleted only if it still matches */
};
int server_reclaim_init(struct efsd_server *s); /* after the usage inventory */
void server_reclaim_shutdown(struct efsd_server *s);
/* Journal, fence, credit; rc[i] = EFS_OK (accepted) or EFS_ERR_BUSY. */
int server_reclaim_accept(struct efsd_server *s, const struct efs_reclaim_item *items,
                          uint32_t n, int32_t *rc);
int server_reclaim_fenced(efs_export_id_t eid, efs_ino_t ino); /* 1 = fenced */
void server_reclaim_status(struct efsd_server *s, struct efs_reclaim_status *out);
/* Physical admission guard for a fragment at `path`, called with s->lock
 * held. 0 = admitted (and charged to that filesystem). */
int server_phys_admit_locked(struct efsd_server *s, const char *path, uint64_t bytes);
void server_phys_sample(struct efsd_server *s);
/* store.c helpers used by reclaim.c. */
pthread_rwlock_t *server_inode_guard(efs_export_id_t eid, efs_ino_t ino);
void server_inode_gc_epoch_bump(void);
int server_gc_death_set_many(struct efsd_server *s, efs_export_id_t eid,
                             const efs_ino_t *inos, uint32_t n);
int server_gc_inode_payload(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                            uint64_t *payload);
/* Delete every fragment of a fenced inode on all roots. counted == NULL debits
 * `used` per unlink; otherwise removed payload is added to *counted (also on
 * failure) and the caller settles quota. durable == 0 skips directory syncs:
 * the caller must server_gc_sync_roots before relying on the removal. */
int server_gc_inode_delete(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                           uint64_t *counted, int durable);
int server_gc_sync_roots(struct efsd_server *s);
/* Delete one superseded fragment generation if its checksum still matches
 * (a mismatch means the slot was reused: nothing to delete), then remove the
 * chunk and inode directories if they are now empty. Debits `used`. */
int server_gc_superseded_delete(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                                uint32_t ci, uint32_t fi, uint64_t gen,
                                const uint8_t sum[EFS_HASH_SIZE]);

/* ---- W125 packed segment store (src/server/segment_store.c) ---- */
struct efs_pack_object;
pthread_rwlock_t *server_segment_guard(uint32_t group, uint64_t id);
void server_segment_path(const struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                         uint32_t fi, char *out, size_t n);
int server_segment_put(struct efsd_server *s, const uint8_t *body, uint32_t bytes);
int server_segment_get(struct efsd_server *s, const uint8_t *req, uint32_t bytes,
                       uint8_t **reply, uint32_t *reply_len);
int server_segment_read(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                        uint32_t fi, uint32_t len, uint8_t *out, uint8_t sum[EFS_HASH_SIZE]);
uint64_t server_segment_payload(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen);
int server_segment_delete(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                          uint64_t *removed);
int server_segment_scan(struct efsd_server *s,
                        void (*fn)(void *, uint32_t, uint64_t, uint64_t), void *arg);
/* Linearizable read of a segment object from its group (ReadIndex, then the
 * local replica). NOT_FOUND when absent; INVAL when the group is not hosted. */
int server_raft_host_pack_object(uint32_t group, uint64_t id, uint64_t gen,
                                 struct efs_pack_object *out);
/* 1 = this node leads the group; term and commit index (optional). */
int server_raft_host_pack_leader(uint8_t group, uint64_t *term, uint64_t *commit);
struct efs_kv *server_raft_host_kv(void);
uint64_t server_raft_host_group_commit(uint8_t group);
uint32_t server_raft_host_group_voters(uint8_t group);
/* Propose a packer command to its group and wait for this replica's apply
 * verdict. */
int server_raft_host_pack_propose(uint8_t group, const uint8_t *cmd, uint32_t len);
/* ---- W125 background packer (src/server/packer.c) ---- */
int server_packer_start(struct efsd_server *s);
void server_packer_stop(void);
/* Copy of the packer counters (struct in protocol.h, sent in the
 * pack-compact reply). */
void server_packer_status(struct efs_packer_status *out);
/* efs-mgmt pack-compact: queue `segments` output segments of compaction on
 * each group this node leads (run even while busy: an operator request)
 * and report the fragmentation summary. */
void server_packer_compact_request(const struct efs_msg_pack_compact *req, struct efs_msg_pack_compact_reply *out);
/* Crash injection points (no-op unless built with EFS_FAULTS=1). */
void server_pack_crash_point(const char *point);

#endif
