#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/query_catalog.h"
#include "efs/raft_frame.h"
#include "member_store.h"
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
    int join_refused; /* K33: stopped because the cluster refused this node */

    pthread_t rejoin_tid; /* background rejoin retry thread */
    char rejoin_addr[64]; /* explicit --join target to keep retrying; empty = use persisted peers */
    uint16_t rejoin_port;
    /* K33: a restarted formed member says HELLO to its persisted peers from
     * the rejoin thread (once one answers) instead of before it serves. */
    int rejoin_announce;
    uint16_t discovery_port;  /* K33: UDP port of local-subnet discovery; 0 off */
    char discover_on[64];     /* K33: probe only this interface (name or address) */

    /* Runtime GC progress: atomically updated, reset on daemon restart. */
    uint64_t gc_removed_fragments, gc_removed_file_bytes, gc_removed_payload_bytes;
    uint64_t gc_delete_errors, gc_mismatches, gc_last_delete_us;
    uint64_t gc_pass_start_us, gc_last_pass_us, gc_passes;
    uint64_t gc_sweep_errors, gc_reap_errors, gc_ack_errors, gc_scan_errors;
    uint64_t gc_trunc_seen[2]; /* D25 TRUNC_PENDING inodes at the last pass */
    uint64_t gc_reap_seen[2], gc_orphan_seen[2], gc_pending[2], gc_first_reap[2], gc_first_seen_us[2];
    uint64_t gc_missing_exports;
    /* W154: degraded marks on the anchors this node leads (bit g of
     * degraded_sampled_mask: groups[g] current) and rebuild totals. */
    uint64_t degraded_chunks[2];
    uint32_t degraded_sampled_mask;
    uint64_t rebuilt_fragments, rebuild_failures, rebuild_retired;
    /* K37: segment marks on the anchors this node leads (bit g of
     * segment_sampled_mask: groups[g] current), totals, and (storage side)
     * the groups whose leader completed an audit of this incarnation. */
    uint64_t degraded_segments[2], unrecoverable_segments[2];
    uint32_t segment_sampled_mask, segment_audited_mask;
    uint64_t segments_rebuilt, segment_rebuild_failures, segment_marks_retired;
    uint64_t segment_audit_marked, segment_audit_checked;
    /* D31: orphan records on the anchors this node leads (bit g of
     * put_orphan_sampled_mask: groups[g] finished a scan) and totals. */
    uint64_t put_orphan_pending[2];
    uint32_t put_orphan_sampled_mask;
    uint64_t put_orphan_collected, put_orphan_deleted, put_orphan_failures;
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
    int bench_sync; /* benchmark-only O_SYNC; daemon store builds ignore it */
    /* Shared writer-pool size (0 = inline, <0 = auto from nproc at start). */
    int nwriters;
    int perf; /* run under perf record when starting */
    int strace; /* attach strace -f (EFS_STRACE_PATH / EFS_STRACE_EXPR) */
    int usage_dirty; /* local->used changed; flush meta/usage.bin soon */
    int nodes_dirty; /* membership changed; persist nodes.bin off the lock */
    /* W153: the store under <meta root>/mdraft, owned (flock) for the
     * process's life, its identity and committed membership; incarnation is
     * the durable boot counter advanced at this start (the Raft boot id). */
    struct member_store mstore;
    uint64_t incarnation;

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
/* K13 + W154: EFS_MSG_REBUILD_PUT. Stores the one missing fragment of a
 * degraded stripe only when the mark's group holds exactly that mark. */
int server_rebuild_put(struct efsd_server *,const uint8_t *,uint32_t,uint8_t *);
struct efs_meta_degraded;
void server_degraded_mark_encode(uint8_t *q, const struct efs_meta_degraded *d);
int server_degraded_mark_decode(const uint8_t *q, struct efs_meta_degraded *d);
int server_raft_host_degraded_check(const uint8_t mark[EFS_REBUILD_MARK_LEN]);
/* D31: this member's durable storage session floor for uuid (0: none). */
int server_put_session_floor(struct efsd_server *, const uint8_t uuid[16], uint32_t *floor);
/* D31 + K13: EFS_MSG_PUT_ORPHAN_DELETE (src/server/put_orphan.c). */
int server_put_orphan_delete(struct efsd_server *, const uint8_t *, uint32_t);
/* The ticket's lane group, after a ReadIndex: OK when its orphan record
 * exists behind the shard's rejection floor (EFS_MD_REQ_PUT_ORPHAN_STATUS). */
int server_raft_host_put_orphan_status(const uint8_t ticket[216]);
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
    uint32_t got = ex ? __atomic_load_n(&ex->chunk_size, __ATOMIC_ACQUIRE) : 0;
    uint32_t cs = (ex && efs_chunk_size_valid(got)) ? got : EFS_DEFAULT_CHUNK_SIZE;
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
/* Fragment file I/O is O_DIRECT only (D42), with no buffered fallback. */
int server_frag_open(const char *path, int flags, mode_t mode);
ssize_t server_frag_pread(int fd, void *out, size_t len, off_t off);
/* EFS_OK when dir's filesystem opens, writes and reads back an aligned page
 * with O_DIRECT; otherwise why names the failing step and errno. */
int server_storage_direct_io_check(const char *dir, char *why, size_t why_len);
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
/* I25 refusals (GET/GC) since start; each one is also logged. */
uint64_t server_fragment_refusals(void);
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

void server_gc_status(struct efsd_server *s, struct efs_msg_gc_status_reply *out);

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
/* -2: the cluster is formed and this node can never be a member (W153/K33:
 * its id or endpoint belongs to another store, or another cluster). Never
 * retried. -3: not a member yet, its addition is pending (K33): retried. */
#define EFS_JOIN_REFUSED_FORMED -2
#define EFS_JOIN_PENDING -3
/* K33: a joining store got its node id (promise persisted, or the committed
 * revision naming it learned); s->id, s->addr and s->port are set. */
#define EFS_JOIN_ADMITTED -4
int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port);
/* K33: one HELLO of a joining store (no node id) at s->addr:s->port to
 * peer: EFS_JOIN_PENDING (keep asking), EFS_JOIN_ADMITTED, EFS_JOIN_REFUSED_FORMED
 * or -1 (unreachable, another build). *note says what answered (zero
 * when nothing did). */
struct efs_join_note {
    uint64_t cluster;
    uint32_t count;  /* committed members, proposal size or stores joined so far */
    uint32_t formed;
    uint32_t npeers; /* other members' endpoints the answer named */
    struct { char addr[64]; uint16_t port; } peer[EFS_MAX_NODES];
};
int server_join_as_new(struct efsd_server *s, const char *peer_host, uint16_t peer_port,
                       struct efs_join_note *note);

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
/* K1/W145: handlers close their connection before the next request. */
void server_handlers_begin_stop(void);
int server_handlers_stopping(void);
/* K34: a stop signal arrived or the handlers began to stop. */
int server_stop_requested(void);
/* EFS_FAULTS builds: parks the named worker forever when
 * EFS_FAULT_SHUTDOWN_PARK names it; production returns 0 at once. */
int server_shutdown_fault_park(const char *worker);
/* Copy one encoded efs_raft_msg into the pump inbox. Delivery ack is the
 * caller's job (EFS_MSG_RAFT_REPLY). BUSY accepts no frame; Raft retries. */
int server_raft_host_inbox(const uint8_t *payload, uint32_t plen);
void server_raft_host_mkfs(struct efs_msg_raft_mkfs_reply *out);
/* Non-empty RAFT_MKFS payload: group byte + command (or group-only =
 * ReadIndex). Leader-only; NOT_PRIMARY otherwise. */
void server_raft_host_submit(const uint8_t *payload, uint32_t plen,
                             struct efs_msg_raft_mkfs_reply *out);
#if EFS_BUILD_EXPIRY
void server_raft_host_expiry_status(struct efs_expiry_status *out);
#endif
void server_raft_host_status(struct efs_msg_raft_status_reply *out);
void server_raft_host_resync(const struct efs_msg_raft_resync *in, struct efs_msg_raft_resync_reply *out);
/* K36 verified recovery (raft_host.c): group slot gi (0..1) is group
 * gi*2; *hosted when this node serves it. prove: OK once a ReadIndex from
 * this node proved its replica applied to the leader's commit (BUSY while
 * a resync from the leader runs, NOT_FOUND when not hosted). scan: one page
 * of the group's inventory for `node` (EFS_RECOVERY_SCAN_*), local after a
 * ReadIndex or from a hosting member. mark: propose EFS_MD_CMD_DEGRADED_MARK,
 * returns the apply verdict. */
int server_raft_host_recovery_group(int gi, uint8_t *group, int *hosted);
int server_raft_host_recovery_prove(uint8_t group, char *why, size_t n);
int server_raft_host_recovery_scan(uint8_t group, uint32_t what, efs_node_id_t node,
                                   struct efs_recovery_cursor *cur, uint8_t *recs, uint32_t *n);
void server_raft_host_recovery_scan_serve(const uint8_t *payload, uint32_t len,
                                          uint8_t **reply, uint32_t *rlen);
struct efs_meta_degraded;
int server_raft_host_recovery_mark(const struct efs_meta_degraded *d);
void server_raft_host_step_down(const struct efs_msg_raft_step_down *in, struct efs_msg_raft_step_down_reply *out);
int server_raft_host_active(void);
/* W153/K33: the committed membership. formed: a revision is committed and
 * learned here (before the host starts, as the member store recorded it).
 * member_hello admits a HELLO (docs/status/k33-membership-protocol-20261023.md
 * M5) from a sender with a node id: EFS_HELLO_REJECT_NONE when both are
 * unformed in one cluster (the sender is a seed or holds a promise) or the
 * sender is the committed member at exactly id, addr:port and store token;
 * _PENDING for a formed sender this unformed node has not learned about;
 * _FORMED when it can never be one (id or endpoint held by another store,
 * another cluster). *count is the committed count (0 unformed).
 * member_hello_join answers a joining store (node id 0, M4'/M5) in ack:
 * reject_reason _PENDING (formation not proposed yet, or its addition is
 * queued), EFS_HELLO_PROMISE with the seed's formation proposal naming it,
 * _NONE with the committed revision naming it (assigned_id its id), or
 * _FORMED (its endpoint or store belongs to another member). ack->cluster
 * names the cluster; node_count the members (or joined stores) so far.
 * member_filter compacts a node table to the committed members (unchanged
 * when unformed) and returns the kept count. keep_members drops HELLO-table
 * rows that are not committed members and adds rows for members the table
 * lacks, from the record (persisted). */
int server_raft_host_formed(void);
/* 1 and a copy of the committed membership this node learned, 0 unformed. */
int server_membership_get(struct efs_membership *out);
/* K33 discovery answer: this node's cluster, member count (or proposal
 * size, or 1 for a seed) and whether it is formed; -1 before the host runs
 * or while joining. */
int server_member_view(uint64_t *cluster, uint32_t *count, int *formed);
uint32_t server_member_hello(efs_node_id_t id, const char *addr, uint16_t port, uint64_t token,
                             uint64_t cluster, uint32_t flags, uint32_t *count);
struct efs_msg_hello_ack;
void server_member_hello_join(const char *addr, uint16_t port, uint64_t token,
                              struct efs_msg_hello_ack *ack);
uint32_t server_member_filter(struct efs_node *nodes, uint32_t count);
void server_nodes_keep_members(struct efsd_server *s, const struct efs_membership *m);
/* K33 test hook: end the process (exit 86) at durable handoff `point` when
 * EFS_TEST_MEMBER_EXIT names it (cluster.c). Unset in every deployment. */
void server_test_member_exit(const char *point);
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
#if EFS_BUILD_EXPIRY
struct efs_expiry_due;
/* Staged conditional transaction. Not connected to RPC or a live worker. */
void server_raft_host_expire(efs_ino_t parent,const char *name,
                             uint64_t committed_now,
                             const struct efs_expiry_due *,
                             struct efs_msg_inode_reply *);
#endif
void server_raft_host_xattr(efs_ino_t ino, uint8_t op, uint32_t flags,
                            const uint8_t *name, uint16_t nlen,
                            const uint8_t *val, uint32_t vlen,
                            const uint8_t *sess_uuid, uint32_t sess_epoch,
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
                             const struct efs_report_pubid *pubid,
                             struct efs_msg_inode_reply *out);
/* W155: the REPORT entry point; runs server_raft_host_report under the
 * request's receipt (session + publication id). */
void server_raft_host_report_receipted(const struct efs_chunk_rec *recs, uint32_t count,
                                       const struct efs_ino_size_rec *irecs,
                                       uint32_t ino_count, uint32_t flags,
                                       const struct efs_opid *session,
                                       const struct efs_report_pubid *pubid,
                                       struct efs_msg_inode_reply *out);
/* W155: EFS_REPORT_F_RECEIPT query; reads the receipt, runs nothing. */
void server_raft_host_report_receipt(const struct efs_opid *session,
                                     const struct efs_report_pubid *pubid,
                                     struct efs_msg_report_receipt_reply *out);
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
/* 0 when every PUT-sync root stopped clean (barrier, syncfs and clean
 * record); -1 when any root's final barrier failed. */
int server_put_sync_stop(struct efsd_server *s);
/* Mark the start of serving; before it, an exit restores each untouched
 * root's previous durability record (server_put_sync_abandon, atexit). */
void server_put_sync_serving(void);
void server_put_sync_abandon(void);

/* K36 verified recovery after an unclean stop (recovery.c drives it; the
 * store primitives are store.c and segment_store.c). */
struct server_recovery_files {
    uint64_t checked, dropped, bytes_dropped, foreign; /* updated atomically */
};
struct efs_frag_ident;
struct efs_recovery_stats;
/* 1 while any attached root is fenced (recovery uncertain). */
int server_put_sync_uncertain(void);
/* OK when every root's durability record names this boot as running. */
int server_put_sync_identity(struct efsd_server *s, char *why, size_t n);
/* Recovery verified: clear every root's fence (COMMIT allowed again). */
void server_put_sync_recovered(void);
/* Prove one fragment file: OK, NOT_FOUND (absent) or the refusal. */
int server_recovery_prove_file(const char *path, struct efs_frag_ident *want,
                               const uint8_t *expect, uint64_t *bytes);
int server_recovery_drop(struct efsd_server *s, const char *path, uint64_t bytes, int charged,
                         struct server_recovery_files *c);
/* Every root's copy of one chunk fragment, under the PUT's object lock:
 * unprovable copies (torn, other identity, digest other than `expect`
 * when set) are dropped. OK when a proven copy remains, NOT_FOUND when
 * none does, IO when an unprovable copy could not be removed. */
int server_recovery_fragment_check(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                                   uint32_t ci, uint32_t fi, uint64_t gen,
                                   const uint8_t expect[EFS_HASH_SIZE],
                                   struct server_recovery_files *c);
/* Every local fragment file (chunk and segment) proven or dropped. */
int server_recovery_scan_local(struct efsd_server *s, struct server_recovery_files *c,
                               const int *stop);
int server_segment_recovery_check(struct efsd_server *s, uint32_t group, uint64_t id,
                                  uint64_t gen, uint32_t fi, const uint8_t *expect,
                                  struct server_recovery_files *c);
int server_segment_recovery_scan(struct efsd_server *s, struct server_recovery_files *c,
                                 const int *stop);
/* Start once serving (raft host and packer up); no-op after a clean stop. */
int server_recovery_start(struct efsd_server *s);
void server_recovery_stop(void);
void server_recovery_stats(struct efs_recovery_stats *out);
/* EFS_FAULTS builds: EFS_FAULT_CRASH=<point>[:<n>] exits 137 at the nth
 * pass of that point (default the first); points put-acked,
 * publish-apply, reclaim-accepted, recovery-local, recovery-marked. A
 * normal build compiles every point out. */
#if EFS_FAULTS
/* Weak: a storage unit test that includes reclaim.c without recovery.c
 * links with no hook. */
void server_fault_crash(const char *point) __attribute__((weak));
#define server_fault_crash_point(p) do { if (server_fault_crash) server_fault_crash(p); } while (0)
#else
#define server_fault_crash_point(p) ((void)0)
#endif


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
                                const uint8_t sum[EFS_HASH_SIZE], int durable);

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
/* K37 storage side of EFS_MSG_SEGMENT_AUDIT: *reply (malloc) is the whole
 * reply frame. Never fails the connection: refusals are in its rc. */
void server_segment_audit(struct efsd_server *s, const uint8_t *req, uint32_t bytes,
                          uint8_t **reply, uint32_t *reply_len);
/* Local verdict for one placed fragment (EFS_SEGMENT_AUDIT_OK, ...). */
uint32_t server_segment_check(struct efsd_server *s, uint32_t group, uint64_t id, uint64_t gen,
                              uint32_t fi, uint32_t geometry, const uint8_t sum[EFS_HASH_SIZE]);
/* ---- K37 segment rebuild on return (src/server/segment_rebuild.c) ----
 * One pass per group this node leads, from the GC loop: audit members
 * that returned with a new incarnation, then rebuild marked segments. */
void server_segment_rebuild_pass(struct efsd_server *s, uint8_t group);
/* Commit a degraded mark for fragment fi (reason EFS_PACK_DEGRADED_R_*)
 * through the group's log; EFS_OK only when the local replica has it. */
int server_segment_mark(uint8_t group, uint64_t id, uint64_t gen, uint32_t fi, uint32_t reason);
/* The same mark from any member (forwarded to the group's leader); returns
 * the group's apply verdict (STALE: the segment is not ACTIVE). */
int server_segment_mark_commit(uint8_t group, uint64_t id, uint64_t gen, uint32_t fi,
                               uint32_t reason);
/* Leadership lost: forget the group's audit and rebuild progress. */
void server_segment_rebuild_reset(uint8_t group);
/* K38: the group's degraded-segment marks (total; lost: two or more
 * fragments) counted in the local replica at the call, for the status
 * reply of a group this node leads. */
int server_segment_degraded_now(uint8_t group, uint64_t *total, uint64_t *lost);
/* 1 while EFS_FAULT_REBUILD_SAMPLE_HOLD holds the rebuild passes (EFS_FAULTS
 * builds; always 0 otherwise). */
int server_test_rebuild_sample_held(void);
/* 0 while any storage root's recovery after an unclean stop is uncertain. */
int server_put_sync_settled(void);
/* Linearizable read of a segment object from its group (ReadIndex, then the
 * local replica). NOT_FOUND when absent; INVAL when the group is not hosted.
 * _state also returns the segment's degraded-mark mask at the same read
 * (0 unmarked, EFS_PACK_READ_DEGRADED_BAD for an unreadable mark). */
/* K28/W151: answer EFS_MSG_PACK_OBJECT_READ (hosting voters only). */
void server_raft_host_pack_object_serve(const struct efs_msg_pack_object_read *q,
                                        struct efs_msg_pack_object_read_reply *r);
int server_raft_host_pack_object(uint32_t group, uint64_t id, uint64_t gen,
                                 struct efs_pack_object *out);
int server_raft_host_pack_state(uint32_t group, uint64_t id, uint64_t gen,
                                struct efs_pack_object *out, uint32_t *degraded);
/* 1 = this node leads the group; term and commit index (optional). */
int server_raft_host_pack_leader(uint8_t group, uint64_t *term, uint64_t *commit);
struct efs_kv *server_raft_host_kv(void);
int server_raft_host_gc_running(void);
uint64_t server_raft_host_group_commit(uint8_t group);
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
