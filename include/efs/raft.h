#ifndef EFS_RAFT_H
#define EFS_RAFT_H

#include "efs/common.h"

/* Single-shard Raft SM (architecture.md §7.1 / §10 steps 4–6).
 * Pure: no sockets, no threads. The caller delivers messages and ticks.
 * Persistent state is behind efs_raft_store. Apply is a callback into the
 * KV SM. Membership changes through joint consensus (I18); a replacement
 * replica is a learner until it has caught up. Clocks are never a
 * correctness input — election timeouts are tick counts the caller sets.
 *
 * I1 election uniqueness · I2 leader completeness · I3 term fencing ·
 * I4 quorum ack (a partitioned leader cannot advance commitIndex) ·
 * I18 overlapping-quorum reconfiguration (desired placement ≠ actual). */

#define EFS_RAFT_FOLLOWER  0
#define EFS_RAFT_CANDIDATE 1
#define EFS_RAFT_LEADER    2

#define EFS_RAFT_MAX_PEERS 7 /* RF = 2f+1, f ≤ 3 */

#define EFS_RAFT_MSG_VOTE_REQ 1
#define EFS_RAFT_MSG_VOTE_REP 2
#define EFS_RAFT_MSG_AE_REQ   3
#define EFS_RAFT_MSG_AE_REP   4
/* InstallSnapshot chunk. last_log_* = lastIncluded. prev_index = byte
 * offset into the user blob. success = 1 on the last chunk. entries[0]
 * is the chunk; offset 0 starts with app_old:4, app_new:4, then user
 * bytes. SNAP_REP prev_index is the next offset; vote_granted = 1 once
 * the follower has installed the snapshot. */
#define EFS_RAFT_MSG_SNAP_REQ 5
#define EFS_RAFT_MSG_SNAP_REP 6

#define EFS_RAFT_GROUP_SHARD  0
#define EFS_RAFT_GROUP_CTRL   1
#define EFS_RAFT_GROUP_SHARD2 2 /* even-shard metadata group (sim step 7) */

/* Odd shards (incl. ROOT ino 1) → group 0; even → group 2. Same mapping
 * the simulator uses; production must not drift. */
static inline uint8_t efs_raft_shard_group(uint32_t shard)
{
    return (shard & 1u) ? EFS_RAFT_GROUP_SHARD : EFS_RAFT_GROUP_SHARD2;
}

/* Internal log commands; never passed to the user apply callback. */
#define EFS_RAFT_CMD_JOINT 0xC1 /* old:u32 BE, new:u32 BE */
#define EFS_RAFT_CMD_COLD  0xC2 /* cfg:u32 BE */

struct efs_raft_entry {
    uint64_t term;
    uint32_t clen;
    const uint8_t *cmd;
};

/* AppendEntries batching: a leader catching up a behind follower used to
 * send ONE log entry per AE, so recovery from a multi-thousand-entry lag
 * needed that many synchronous round-trips (~9 entries/s observed when the
 * follower was busy) — the follower never caught up under load and the
 * steady heartbeat/commit/ReadIndex broadcasts then filled its outbox and
 * dropped, trapping it. send_ae now batches up to EFS_RAFT_AE_MAX entries
 * (byte-capped at EFS_RAFT_AE_BYTES) per AE so a lagging follower recovers
 * in a few round-trips. */
#define EFS_RAFT_AE_MAX   128u             /* max entries per AppendEntries */
#define EFS_RAFT_AE_BYTES (1024u * 1024u)  /* max cmd bytes per AE batch */
#define EFS_RAFT_SNAP_CHUNK (4u * 1024u * 1024u) /* ≤ wire SNAP cmd cap */

struct efs_raft_msg {
    uint8_t type;
    uint8_t group;
    int from;
    int to;
    uint64_t term;
    uint64_t boot_id; /* process incarnation; stale packets drop */
    uint64_t last_log_index; /* AE request/reply: echoed read-round context */
    uint64_t last_log_term;
    int vote_granted;
    uint64_t prev_index;
    uint64_t prev_term;
    uint64_t leader_commit;
    uint64_t match_index;
    int success;
    uint32_t nentries;
    struct efs_raft_entry entries[EFS_RAFT_AE_MAX]; /* batched AE payloads */
    /* W19: entry bytes already sit in this buffer at EFS_WIRE_RAFT_HDR_LEN.
     * The send callback writes the header. host_send takes the pointer
     * (sets wire to NULL). Any other callback leaves it; send_ae frees it. */
    uint8_t *wire;
    uint32_t wire_len;
};

struct efs_raft_store {
    int (*save_hard)(void *ctx, uint64_t current_term, int32_t voted_for);
    int (*load_hard)(void *ctx, uint64_t *current_term, int32_t *voted_for);
    int (*append)(void *ctx, uint64_t index, uint64_t term,
                  const uint8_t *cmd, uint32_t clen);
    int (*truncate_from)(void *ctx, uint64_t index);
    int (*get)(void *ctx, uint64_t index, uint64_t *term, uint8_t *cmd,
               uint32_t *clen);
    int (*last)(void *ctx, uint64_t *index, uint64_t *term);
    int (*save_snap)(void *ctx, uint64_t last_index, uint64_t last_term);
    int (*load_snap)(void *ctx, uint64_t *last_index, uint64_t *last_term);
    /* Optional. Snapshot drops the log prefix, so the applied config must
     * live here. Missing load is treated as "never saved". */
    int (*save_cfg)(void *ctx, uint32_t cfg_old, uint32_t cfg_new);
    int (*load_cfg)(void *ctx, uint32_t *cfg_old, uint32_t *cfg_new);
    void (*destroy)(void *ctx);
    /* Optional. One fsync for a multi-entry AppendEntries. NULL means
     * each append fsyncs on its own. batch_end returns only once this
     * thread's appends are durable, including when other threads are
     * still holding the shared log fsync. */
    int (*batch_begin)(void *ctx);
    int (*batch_end)(void *ctx);
    /* Optional. First index still in the log. Absent means snap_idx+1,
     * the point where a snapshot has dropped every earlier entry.
     * A retained window reports an index at or below snap_idx. */
    uint64_t (*log_floor)(void *ctx);
    /* Optional. Bytes of log commands with index > snap_idx. The host
     * snapshots when this reaches EFS_RAFT_SNAP_BYTES. */
    uint64_t (*log_new_bytes)(void *ctx);
};

/* Snapshot once the log has grown by this many command bytes, and keep
 * that much of the log afterwards so a follower inside the window
 * catches up from AppendEntries. Internal: not a knob. */
#define EFS_RAFT_SNAP_BYTES (512ull << 20)

typedef int (*efs_raft_send_fn)(void *net, const struct efs_raft_msg *msg);
typedef int (*efs_raft_apply_fn)(void *app, uint64_t index, uint64_t term,
                                 const uint8_t *cmd, uint32_t clen);
/* Snapshot of applied index `incl`. open returns a handle and the user
 * blob length. BUSY = the bytes are not readable yet (export in
 * progress); NOT_FOUND = this index has no snapshot, try last_applied.
 * read copies n bytes at offset. close drops the handle.
 * chunk is the follower: write user bytes at offset (offset 0 restarts
 * the staging file). done=1 means this chunk finishes the blob and the
 * callback imports it before returning. NULL open → metadata-only
 * SNAP; NULL chunk rejects a skip-ahead. */
typedef int (*efs_raft_snap_open_fn)(void *app, uint64_t incl, void **handle,
                                     uint64_t *total_len);
typedef int (*efs_raft_snap_read_fn)(void *handle, uint64_t offset,
                                     uint8_t *buf, uint32_t n, uint32_t *got);
typedef void (*efs_raft_snap_close_fn)(void *handle);
typedef int (*efs_raft_snap_chunk_fn)(void *app, uint64_t incl,
                                      uint64_t incl_term, uint64_t offset,
                                      const uint8_t *data, uint32_t len,
                                      int done);

struct efs_raft_cfg {
    int id; /* 0 .. EFS_RAFT_MAX_PEERS-1; may be outside voters (learner) */
    int n;  /* bootstrap replica count; voters=0 → (1u<<n)-1 */
    uint32_t voters; /* committed voting set; 0 = default from n */
    uint32_t election_ticks; /* > heartbeat_ticks */
    uint32_t heartbeat_ticks;
    uint64_t boot_id; /* 0 → 1 */
    /* Seed for the randomized election timeout. The election deadline is
     * redrawn from [election_ticks, 2*election_ticks) on every reset so
     * synchronized followers cannot split the vote forever (a fixed timeout
     * lets a behind-log, shorter-timeout candidate livelock the up-to-date
     * follower by repeatedly resetting its timer via maybe_step_down).
     * Injected (not rand()) so the deterministic simulator stays
     * replayable: the sim passes a seed from its own PRNG, the host passes
     * entropy. 0 → derived from id/boot_id/group. */
    uint64_t rng_seed;
    uint8_t group;
    struct efs_raft_store *store;
    void *store_ctx;
    efs_raft_send_fn send;
    void *net;
    efs_raft_apply_fn apply;
    void *app;
    efs_raft_snap_open_fn snap_open;
    efs_raft_snap_read_fn snap_read;
    efs_raft_snap_close_fn snap_close;
    efs_raft_snap_chunk_fn snap_chunk;
    /* User bytes per chunk. 0 → EFS_RAFT_SNAP_CHUNK. Chunk 0 also
     * carries the 8-byte config prefix inside the same 4 MiB cap. */
    uint32_t snap_chunk_bytes;
};

struct efs_raft;

struct efs_raft *efs_raft_new(const struct efs_raft_cfg *cfg);
void efs_raft_free(struct efs_raft *r);

int efs_raft_tick(struct efs_raft *r);
int efs_raft_recv(struct efs_raft *r, const struct efs_raft_msg *msg);
int efs_raft_propose(struct efs_raft *r, const uint8_t *cmd, uint32_t clen,
                     uint64_t *index_out);
/* Append only. Pair with efs_raft_submit (raise the send ceiling) before
 * the fsync and efs_raft_durable (leader may vote) after it. The pump
 * calls efs_raft_flush to replicate; submit itself does not send, so
 * other appends join that batch. Caller holds the lock. Not for a
 * 1-voter group. */
int efs_raft_propose_local(struct efs_raft *r, const uint8_t *cmd,
                           uint32_t clen, uint64_t *index_out);
void efs_raft_arm_durable(struct efs_raft *r);
int efs_raft_submit(struct efs_raft *r, uint64_t idx);
/* Send every entry covered by efs_raft_submit, one batch per peer.
 * No empty heartbeat. Caller holds the lock. */
int efs_raft_flush(struct efs_raft *r);
int efs_raft_durable(struct efs_raft *r, uint64_t idx);
/* Compact log prefix through last_applied. snap_open (if set) captures
 * the state at that index so InstallSnapshot can rebuild a learner that
 * never applied 1..snap_idx. The bytes may become readable later (BUSY);
 * the snap point is recorded either way. KV already durable through
 * last_applied — no snapshot past that (step 4). */
int efs_raft_snapshot(struct efs_raft *r);
/* Crash restart without dropping the log: KV is already durable through
 * idx, so do not re-apply 1..idx. Does not compact. idx is clamped to
 * [snap_idx, last log]. */
int efs_raft_restore_applied(struct efs_raft *r, uint64_t idx);

int efs_raft_role(const struct efs_raft *r);
uint64_t efs_raft_term(const struct efs_raft *r);
uint64_t efs_raft_commit(const struct efs_raft *r);
uint64_t efs_raft_applied(const struct efs_raft *r);
uint64_t efs_raft_snap_index(const struct efs_raft *r);
/* First index the log can still serve. snap_idx+1 when the store keeps
 * no window. InstallSnapshot is for a peer below this. */
uint64_t efs_raft_log_floor(const struct efs_raft *r);
/* 1 when the store reports log_new_bytes. */
int efs_raft_tracks_log_bytes(const struct efs_raft *r);
uint64_t efs_raft_log_new_bytes(const struct efs_raft *r);
int efs_raft_leader(const struct efs_raft *r); /* -1 if unknown */

/* Committed voting set (C_old). During joint this is still C_old. */
uint32_t efs_raft_voters(const struct efs_raft *r);
/* 1 if a joint config is committed (C_new not yet COLD-committed). */
int efs_raft_joint(const struct efs_raft *r);
/* Leader-only. new_voters must be odd, 1..MAX bits. Ids not in C_old are
 * added as learners; returns BUSY until each has match_index >= commit.
 * Always goes through joint consensus — there is no skip-joint path. */
int efs_raft_change(struct efs_raft *r, uint32_t new_voters);
/* Voluntary follower. Does not bump the term. allow_campaign=0 stops
 * start_election so a replica whose apply is missing committed rows cannot
 * win again (same last_log as a complete peer). */
int efs_raft_step_down(struct efs_raft *r);
void efs_raft_allow_campaign(struct efs_raft *r, int on);

/* ReadIndex: leader records commitIndex, waits for a majority heartbeat
 * in the current term, then applied >= that index. No clock leases. */
int efs_raft_read_begin(struct efs_raft *r);
int efs_raft_read_ready(const struct efs_raft *r);
/* Inspection of the most recently begun round, not authority for a new
 * request. Callers must admit before starting their eligible round. */
int efs_raft_read_current(const struct efs_raft *r);
/* A reader captures read_round+1 at arrival. It may share a later round
 * with other previously admitted readers, but cannot join a round whose
 * probes were sent before its arrival. Hosts queue rather than restart a
 * pending round; explicit core restarts discard its acknowledgements and
 * allocate a new context. No clock leases. */
uint64_t efs_raft_read_round(const struct efs_raft *r);
int efs_raft_read_pending(const struct efs_raft *r);
int efs_raft_read_covers(const struct efs_raft *r, uint64_t want);
/* Raw round state for a host that publishes a lock-free view: the round's
 * index and whether it has its quorum (ready once applied >= that index). */
uint64_t efs_raft_read_index(const struct efs_raft *r);
int efs_raft_read_done(const struct efs_raft *r);

struct efs_raft_store *efs_raft_mem_create(void);
void efs_raft_mem_free(struct efs_raft_store *st);

#endif
