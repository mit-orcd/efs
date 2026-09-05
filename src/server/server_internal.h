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
    /* Per-shard metadata locks: shard_locks[eidx * EFS_META_MAX_SHARDS + shard].
     * Heap-allocated (EFS_MAX_EXPORTS * EFS_META_MAX_SHARDS mutexes) at startup
     * so the array survives export rebuilds (which efs_export_free + struct-copy
     * the export slot and would destroy an inline lock). Lock ordering is
     * strictly global (s->lock) -> shard; a handler must never hold a shard
     * lock while acquiring s->lock. Flush/rebuild take s->lock plus ALL shard
     * locks of the export. */
    pthread_mutex_t *shard_locks;
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

    /* Group commit for the fsync flush (guarded by flush_grp_mu). Every
     * synchronous REPORT_CHUNKS runs a full flush, so 36-way posixstress
     * issued ~55 flushes/s of ~14 ms each against the single meta_flush_mu —
     * ~77% duty on one serialized resource, which queued each flush ~88 ms
     * (85% of the measured flush window was pure wait). Concurrent fsyncs
     * share one flush instead: a caller that finds a flush already running
     * waits for the NEXT one, which is guaranteed to snapshot after its
     * mutation was applied. Per export slot, since a flush only covers the
     * export it ran on. */
    pthread_mutex_t flush_grp_mu;
    pthread_cond_t flush_grp_cv;
    uint64_t flush_started[EFS_MAX_EXPORTS];
    uint64_t flush_done[EFS_MAX_EXPORTS];
    /* Highest flush generation any queued caller still needs. The leader keeps
     * flushing until flush_done reaches it — a waiter count cannot be used
     * here, since waiters only decrement it after the leader drops the mutex,
     * which would spin the leader on flushes nobody needs. */
    uint64_t flush_target[EFS_MAX_EXPORTS];
    int flush_running[EFS_MAX_EXPORTS];
    int flush_last_rc[EFS_MAX_EXPORTS];

    /* Open-fd refs + cluster flock (guarded by s->lock). */
    struct efs_ino_hold *ino_holds;

    /* 2PC pending root (per export slot): a gen-advancing root received via
     * PUT_META (prepare) that the writer has NOT committed yet. Stashed here
     * untouched — not installed, persisted, fenced, or GC'd — until
     * EFS_MSG_META_COMMIT arrives for its generation. A newer prepare for the
     * same export replaces it (same-gen retry after a failed quorum writes
     * fresh cis; the superseded pending root's pages are unreferenced and
     * simply overwritten). Cleared on promote or on catchup install.
     * Guarded by s->lock. */
    struct efs_export_root pending_root[EFS_MAX_EXPORTS];
    uint8_t pending_valid[EFS_MAX_EXPORTS];
    /* Raw prepare payload (serialized root) stashed alongside pending_root:
     * META_COMMIT carries efs_hash(root bytes) so a same-gen retry with new
     * content at the same cis can never promote a superseded prepare. */
    uint8_t *pending_blob[EFS_MAX_EXPORTS];
    uint32_t pending_blob_len[EFS_MAX_EXPORTS];

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

/* Per-shard metadata lock helpers. Ordering is strictly global (s->lock)
 * -> shard, and a multi-shard op locks in ascending shard id, so concurrent
 * handlers can never deadlock. A handler must never hold a shard lock while
 * acquiring s->lock (release the shard lock first). Flush/rebuild hold
 * s->lock and take ALL shard locks of the export via server_shard_lock_all. */

/* EFS_LOCK_PROF accounting. Shard-lock waits are off-CPU (futex), so
 * `perf record` cannot see any of this. Counters are relaxed atomics —
 * contention on them would itself distort the measurement. */
extern int efs_lock_prof_on;
extern unsigned long long efs_lock_all_calls;
extern unsigned long long efs_lock_all_wait_us;
extern unsigned long long efs_lock_all_shards;
/* How long lock_all HOLDS every shard. This is the real cost of the
 * transitional handlers: while one holds all N shards, every per-op handler
 * (CREATE/APPEND, which take only their own shards) is blocked, and the hold
 * widens with shard_count. Acquire time alone hides this entirely. */
extern unsigned long long efs_lock_all_hold_us;
/* Per-op shard-lock wait — what a per-op handler pays queueing behind a
 * transitional lock_all. */
extern unsigned long long efs_lockn_calls;
extern unsigned long long efs_lockn_wait_us;
/* Single-shard server_shard_lock (LOOKUP/GETATTR/READDIR). lockn does not
 * cover these; they were invisible in n_wait_us. */
extern unsigned long long efs_lock1_calls;
extern unsigned long long efs_lock1_wait_us;
/* g_server->lock wait+hold on the inode-handler / REPORT sites. */
extern unsigned long long efs_global_calls;
extern unsigned long long efs_global_wait_us;
extern unsigned long long efs_global_hold_us;
extern unsigned long long efs_busy_replies;
extern __thread unsigned long long efs_lock_all_t0;
extern __thread unsigned long long efs_global_t0;
/* Per-opcode RPC counts. Shard count changes how a client's dirty set and
 * lookups partition across owners, so it changes the RPC count for the same
 * workload — that is invisible in any server-side lock or CPU measurement. */
extern unsigned long long efs_rpc_count[256];

static inline unsigned long long server_lock_prof_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull +
           (unsigned long long)ts.tv_nsec / 1000ull;
}

static inline pthread_mutex_t *server_shard_mu(struct efsd_server *s,
                                               uint32_t eidx,
                                               uint32_t shard) {
    return &s->shard_locks[(size_t)eidx * EFS_META_MAX_SHARDS + shard];
}

static inline void server_shard_lock(struct efsd_server *s, uint32_t eidx,
                                     uint32_t shard) {
    if (!efs_lock_prof_on) {
        pthread_mutex_lock(server_shard_mu(s, eidx, shard));
        return;
    }
    unsigned long long t0 = server_lock_prof_us();
    pthread_mutex_lock(server_shard_mu(s, eidx, shard));
    __atomic_add_fetch(&efs_lock1_wait_us, server_lock_prof_us() - t0,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_lock1_calls, 1, __ATOMIC_RELAXED);
}

/* Timed g_server->lock for inode RPC + REPORT. Other sites (HELLO, PUT,
 * flush) stay on the raw mutex so this split is the handler queue. */
static inline void server_global_lock(struct efsd_server *s) {
    if (!efs_lock_prof_on) {
        pthread_mutex_lock(&s->lock);
        return;
    }
    unsigned long long t0 = server_lock_prof_us();
    pthread_mutex_lock(&s->lock);
    unsigned long long t1 = server_lock_prof_us();
    __atomic_add_fetch(&efs_global_wait_us, t1 - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_global_calls, 1, __ATOMIC_RELAXED);
    efs_global_t0 = t1;
}

static inline void server_global_unlock(struct efsd_server *s) {
    if (efs_lock_prof_on && efs_global_t0) {
        __atomic_add_fetch(&efs_global_hold_us,
                           server_lock_prof_us() - efs_global_t0,
                           __ATOMIC_RELAXED);
        efs_global_t0 = 0;
    }
    pthread_mutex_unlock(&s->lock);
}

static inline void lock_prof_note_busy(uint8_t status) {
    if (efs_lock_prof_on && status == EFS_INODE_RPC_BUSY)
        __atomic_add_fetch(&efs_busy_replies, 1, __ATOMIC_RELAXED);
}

static inline void server_shard_unlock(struct efsd_server *s, uint32_t eidx,
                                       uint32_t shard) {
    pthread_mutex_unlock(server_shard_mu(s, eidx, shard));
}

/* Lock 1..N shards of one export in ascending (deadlock-free) order. Sorts
 * sh[] in place; the caller keeps the original per-role shard ids in separate
 * variables for the op logic and passes the (now sorted) sh[] to unlockn. */
static inline void server_shard_lockn(struct efsd_server *s, uint32_t eidx,
                                      uint32_t *sh, int n) {
    for (int i = 1; i < n; i++) {
        uint32_t v = sh[i];
        int j = i - 1;
        while (j >= 0 && sh[j] > v) { sh[j + 1] = sh[j]; j--; }
        sh[j + 1] = v;
    }
    unsigned long long t0 = efs_lock_prof_on ? server_lock_prof_us() : 0;
    uint32_t prev = UINT32_MAX;
    for (int i = 0; i < n; i++) {
        if (sh[i] == prev) continue;
        pthread_mutex_lock(server_shard_mu(s, eidx, sh[i]));
        prev = sh[i];
    }
    if (efs_lock_prof_on) {
        __atomic_add_fetch(&efs_lockn_wait_us, server_lock_prof_us() - t0,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&efs_lockn_calls, 1, __ATOMIC_RELAXED);
    }
}

static inline void server_shard_unlockn(struct efsd_server *s, uint32_t eidx,
                                        const uint32_t *sh, int n) {
    uint32_t prev = UINT32_MAX;
    for (int i = n - 1; i >= 0; i--) {
        if (sh[i] == prev) continue;
        pthread_mutex_unlock(server_shard_mu(s, eidx, sh[i]));
        prev = sh[i];
    }
}

/* Flush/rebuild: lock every shard of an export (caller already holds
 * s->lock, so global -> shard order is preserved). */
static inline void server_shard_lock_all(struct efsd_server *s, uint32_t eidx,
                                         uint32_t sc) {
    if (!efs_lock_prof_on) {
        for (uint32_t i = 0; i < sc; i++)
            pthread_mutex_lock(server_shard_mu(s, eidx, i));
        return;
    }
    unsigned long long t0 = server_lock_prof_us();
    for (uint32_t i = 0; i < sc; i++)
        pthread_mutex_lock(server_shard_mu(s, eidx, i));
    efs_lock_all_t0 = server_lock_prof_us();
    __atomic_add_fetch(&efs_lock_all_wait_us, efs_lock_all_t0 - t0,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_lock_all_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&efs_lock_all_shards, sc, __ATOMIC_RELAXED);
}

static inline void server_shard_unlock_all(struct efsd_server *s, uint32_t eidx,
                                           uint32_t sc) {
    if (efs_lock_prof_on && efs_lock_all_t0) {
        __atomic_add_fetch(&efs_lock_all_hold_us,
                           server_lock_prof_us() - efs_lock_all_t0,
                           __ATOMIC_RELAXED);
        efs_lock_all_t0 = 0;
    }
    for (uint32_t i = 0; i < sc; i++)
        pthread_mutex_unlock(server_shard_mu(s, eidx, i));
}

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

/* NVMe adapter for efs/store.h. Bind to an already-acquired export. */
struct efs_nvme_store {
    struct efsd_server *s;
    struct efs_export *ex;
};
struct efs_store;
void efs_store_nvme_bind(struct efs_store *st, struct efs_nvme_store *ctx,
                         struct efsd_server *s, struct efs_export *ex);

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
/* Group-commit wrapper: concurrent fsync flushes of the same export share one
 * flush instead of each running a full serialized one. Use this on the fsync
 * path; the flush thread and migrate call the plain version. */
int server_flush_meta_grouped(struct efsd_server *s, struct efs_export *ex);

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
/* Send metadata to all peers. Returns number of acks. */
int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex);

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

/* Production Raft host (architecture.md §10 10.5c). Env-gated: a no-op
 * unless EFS_MD_RAFT is set. Start fails loud on setup error so a broken
 * host cannot look like a healthy 2PC node. Stop is safe if never started. */
int server_raft_host_start(struct efsd_server *s);
void server_raft_host_stop(void);
/* Copy one encoded efs_raft_msg into the pump inbox. Delivery ack is the
 * caller's job (EFS_MSG_RAFT_REPLY). Drops when the host is off or the
 * inbox is full — Raft retries. */
int server_raft_host_inbox(const uint8_t *payload, uint32_t plen);
void server_raft_host_mkfs(struct efs_msg_raft_mkfs_reply *out);
void server_raft_host_status(struct efs_msg_raft_status_reply *out);

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
