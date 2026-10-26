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

/* Any nonempty voter set; quorum = floor(N/2)+1. Raft id = node id - 1, so
 * the peer space equals the cluster cap: every member may be addressed (a
 * replacement replica can be placed on any node), while the default layout
 * keeps three voters per group (efs_md_group_voters). */
#define EFS_RAFT_MAX_PEERS EFS_MAX_NODES

/* Engine and host must accept exactly the same membership masks. Even sets
 * retain strict majorities; accepting a mask does not select protection policy. */
static inline int efs_raft_voters_valid(uint32_t voters)
{
    return voters != 0 && !(voters >> EFS_RAFT_MAX_PEERS);
}

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
/* Caught-up voter starts a normal election; never grants authority itself. */
#define EFS_RAFT_MSG_TIMEOUT_NOW 7
/* K1/W145 pre-vote (Raft thesis 9.6). A follower whose election timer
 * fires asks first whether a quorum would vote for it at term+1, without
 * changing its own term. term carries the sender's current term (send_msg
 * stamps it); the prospective term travels in prev_term in both the request
 * and the reply. A voter grants only if it has not heard from a leader
 * within the minimum election timeout and the candidate's log is at least
 * as up to date. The request never changes the receiver's term or vote,
 * and neither does a grant; a refusal from a higher term advances the
 * candidate's term to it (as any higher-term reply would) and ends its
 * pre-vote. */
#define EFS_RAFT_MSG_PREVOTE_REQ 8
#define EFS_RAFT_MSG_PREVOTE_REP 9

#define EFS_RAFT_GROUP_SHARD  0
#define EFS_RAFT_GROUP_CTRL   1
#define EFS_RAFT_GROUP_SHARD2 2 /* even-shard metadata group (sim step 7) */

/* K28/W151 + D38 default voter layout for a cluster of n nodes (node ids
 * 1..n, raft id = node id - 1). n <= 3: every node votes in every group.
 * n >= 4: three voters per group, a window of three consecutive nodes that
 * starts at the group's ordinal: the shard group (ordinal 0) on nodes 1,2,3,
 * every other group (ordinal 1, SHARD2) on nodes 2,3,4. This is the
 * four-node layout unchanged; nodes 5..n host no metadata group and serve
 * data and clients only (alpha 1.0: metadata on three voters per group).
 * Operator reconfiguration (CFG CHANGE) may still move a voter anywhere in
 * 0..EFS_RAFT_MAX_PEERS-1. Returns 0 for an n outside 1..EFS_MAX_NODES. */
static inline uint32_t efs_md_group_voters(uint8_t group, int n)
{
    if (n < 1 || n > EFS_MAX_NODES)
        return 0;
    if (n <= 3)
        return (1u << n) - 1u;
    return group == EFS_RAFT_GROUP_SHARD ? 0x7u : 0xeu;
}

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
    /* K22/W23, AE_REP only: this follower is at its unapplied-log bound.
     * On a rejection it refused the batch (nothing appended, nothing
     * dropped: the leader resends from match_index+1). On a success it is
     * advisory: send heartbeats only until a reply clears it. Wire: bit 1
     * of the success byte. */
    int busy;
    uint32_t nentries;
    struct efs_raft_entry entries[EFS_RAFT_AE_MAX]; /* batched AE payloads */
    /* W19: entry bytes already sit in this buffer at EFS_WIRE_RAFT_HDR_LEN.
     * The send callback writes the header. host_send takes the pointer
     * (sets wire to NULL). Any other callback leaves it; send_ae frees it. */
    uint8_t *wire;
    uint32_t wire_len;
};

/* Pure, non-reentrant estimate; stores may call it under their log mutex. */

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
    /* Optional nested per-thread persistence scope. batch_end proves this
     * thread's store mutations durable even while other threads hold a shared
     * log sync. Used by receive cohorts, follower fallback and hard state.
     * With NULL callbacks each mutation must supply its own durable boundary. */
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
/* Optional bounded host queue hint, no I/O. Zero defers building an entry
 * frame; it changes no log/progress state and cannot grant read authority. */
typedef int (*efs_raft_send_ready_fn)(void *net, int peer, uint8_t group);
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

/* Optional follower receive-persistence handoff. The core freezes this group's Raft
 * mutation/read authority while the hook runs. Call persist(persist_ctx) exactly
 * once, synchronously on the receive thread (store deferral can be thread-local),
 * and return with the caller's serialization restored. The cohort and decoded
 * command ownership stay on that thread until durable completion/failed fencing.
 * Other groups may progress; freeing this group still requires joining its owner.
 * Stable ordinary append persistence may answer prefix-zero leadership contact in
 * its already durable term/configuration. This grants no local read authority
 * and acknowledges none of the cohort's pending log bytes.
 */
typedef int (*efs_raft_recv_persist_fn)(void *net,
                                      int (*persist)(void *), void *persist_ctx);

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
    efs_raft_send_ready_fn send_ready;
    efs_raft_recv_persist_fn recv_persist;
    /* A successful ACK below the outstanding window's first index cannot
     * retire that newer window. The server host enables it; 0 keeps the
     * original retirement for the core tests. */
    int preserve_ack_window;
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
/* Process already queued messages in order. Only contiguous, same-leader,
 * same-term new appends share persistence; conflicts, reads, elections and
 * configuration changes flush first. No ACK or apply precedes batch_end.
 * Caller serializes this exactly as recv. Never waits for more messages. */
#define EFS_RAFT_RECV_MAX 16u
int efs_raft_recv_batch(struct efs_raft *r, const struct efs_raft_msg *msgs,
                        uint32_t n);

int efs_raft_propose(struct efs_raft *r, const uint8_t *cmd, uint32_t clen,
                     uint64_t *index_out);
/* Append only. Pair with efs_raft_submit (raise the send ceiling) before
 * the fsync and efs_raft_durable (leader may vote/apply) after it, or
 * efs_raft_note_durable followed by pump-owned commit/apply. The pump
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
/* Same checked durable watermark, without commit/apply callbacks. Hosts
 * with batched KV apply must wake their serialized pump after noting it. */
int efs_raft_note_durable(struct efs_raft *r, uint64_t idx);
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

/* W145: per-peer replication progress as this node sees it (meaningful on
 * the leader), plus counters that name why a peer is not catching up.
 * Ages are in ticks; UINT64_MAX = never. Counters run since start. */
struct efs_raft_peer_progress {
    uint64_t next_index, match_index;
    uint64_t inflight, inflight_end; /* outstanding catch-up batch; 0 = none */
    uint64_t inflight_age, ack_age;
    uint64_t batches_sent;  /* entry batches handed to the transport */
    uint64_t send_again;    /* entry batches refused: peer outbox full */
    uint64_t send_blocked;  /* entry batches not built: host not ready */
    uint64_t send_errors;   /* entry batches failed with another error */
    uint64_t rejects;       /* AppendEntries rejections from this peer */
    uint64_t busy;          /* K22: busy rejections/advisories from this peer */
    int busy_hold;          /* K22: entries withheld from this peer now */
    uint64_t reject_hint;   /* match_index carried by the last rejection */
};
int efs_raft_peer_progress(const struct efs_raft *r, int peer,
                           struct efs_raft_peer_progress *out);
/* This replica's log and follower-side counters. Reads the log tail, so
 * call it from status paths only. */
struct efs_raft_log_progress {
    uint64_t last_index, last_term, snap_index;
    uint64_t rejects_sent;  /* AppendEntries this follower refused */
    uint64_t proto_drops;   /* refused with no reply: conflict at or below commit */
    /* K22/W23 bound: log entries past last_applied (committed or not),
     * its cap (0 = unbounded), batches refused busy as a follower and
     * proposals refused EFS_ERR_BUSY as a leader. */
    uint64_t unapplied, max_unapplied, busy_sent, admit_busy;
    uint64_t low_capacity;     /* 1 while the replica may not lead (pressure
                                * or committed backlog, not the tail) */
    uint64_t campaigns_held;   /* elections not started for capacity */
    uint64_t capacity_stepdowns;
};
int efs_raft_log_progress(struct efs_raft *r, struct efs_raft_log_progress *out);
/* K22/W23 universal follower memory/lag bound. A replica's unapplied log
 * (entries past last_applied, committed or not) never exceeds max entries:
 * a follower refuses an AppendEntries batch that would cross it (busy
 * reply, no drop), a leader refuses proposals with EFS_ERR_BUSY. Heartbeats
 * and commit/apply always proceed, so the backlog drains. 0 = unbounded. */
void efs_raft_set_max_unapplied(struct efs_raft *r, uint64_t max);
/* The cap derived from the replica's memory budget (bytes): each retained
 * unapplied entry costs a resident index record; budget/256, at least two
 * full AppendEntries batches. No operator setting. */
uint64_t efs_raft_unapplied_cap(uint64_t mem_budget);
/* K22/W23 (D30: leadership requires local capacity). The host reports its
 * state machine's memory pressure (KV at the admission watermark). A replica
 * under pressure, or with >= 3/4 of max_unapplied committed but not applied,
 * does not campaign; a leader that stays so for four election timeouts steps
 * down so a healthy peer can lead. The uncommitted part of the log tail does
 * not count: committing it needs a leader, so such a replica campaigns and
 * may lead, while admission (EFS_ERR_BUSY at the cap here, at 3/4 in the
 * host) still counts the whole tail until it is committed and applied.
 * Votes are unaffected; a solo voter never steps down. */
void efs_raft_set_pressure(struct efs_raft *r, int over);
/* W145 repair, operator-triggered only: this follower's committed log
 * disagrees with the leader (a conflict at or below commit). While set,
 * such a conflict is answered with match_index 0 instead of refused
 * silently, so the leader falls back to InstallSnapshot; the fresh install
 * replaces this group's state and log and clears the request.
 * EFS_ERR_NOT_PRIMARY on a leader. */
int efs_raft_request_resync(struct efs_raft *r);
int efs_raft_resync_pending(const struct efs_raft *r);

/* A resyncing peer pulled its match to 0: the pump must export an image
 * for send_snap outside the byte cadence. */
int efs_raft_snap_wanted(const struct efs_raft *r);

/* Committed voting set (C_old). During joint this is still C_old. */
uint32_t efs_raft_voters(const struct efs_raft *r);
/* 1 if a joint config is committed (C_new not yet COLD-committed). */
int efs_raft_joint(const struct efs_raft *r);
/* Leader-only. new_voters must be a valid nonempty mask. Ids not in C_old are
 * added as learners; returns BUSY until each has match_index >= commit.
 * Always goes through joint consensus — there is no skip-joint path. */
int efs_raft_change(struct efs_raft *r, uint32_t new_voters);
/* Voluntary follower. Does not bump the term. allow_campaign=0 stops
 * start_election so a replica whose apply is missing committed rows cannot
 * win again (same last_log as a complete peer). */
int efs_raft_step_down(struct efs_raft *r);
void efs_raft_allow_campaign(struct efs_raft *r, int on);
/* Leader-only, stable configuration, recently responsive caught-up voter.
 * Durable acknowledgment caps apply when configured. Voluntarily
 * steps down before sending a bounded election trigger. OK means submitted,
 * not that the target won; lost messages recover through normal election.
 * Existing proposals keep their exact log identity; new proposals are fenced. */
int efs_raft_transfer(struct efs_raft *r, int target);

/* ReadIndex: leader records commitIndex, waits for a majority heartbeat
 * in the current term, then applied >= that index. No clock leases. */
int efs_raft_read_begin(struct efs_raft *r);
/* Same serialization as recv. Pure stable read control only: 0 fallback,
 * 1 fully accounted request/reply handled, 2 fresh read control noted
 * (request commit/apply or reply replication bookkeeping still needs recv).
 * Caller must preserve pending inbox order and deferred log durability. A pure
 * leadership vote during a stable ordinary cohort does not retire log work. */
int efs_raft_read_control(struct efs_raft *r, const struct efs_raft_msg *msg);
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
/* Current leader's monotonic proof-context identity, zero when unavailable/failed.
 * This is an invalidation stamp, never proof or permission to read. */
uint64_t efs_raft_read_context(const struct efs_raft *r);
/* Correlated fresh quorum proof only; does not authorize reading unapplied KV. */
int efs_raft_read_quorum(const struct efs_raft *r);
/* Quorum completion, preferring a valid applied proof over a newer unapplied
 * target. Two bounded records retain applied and first-unapplied proofs. The
 * caller must require round >= its PRE-SEND admission, the same current
 * term, and applied >= index. Never authority for a newly arriving read.
 * Invalid after term/configuration/role changes or storage failure. */
int efs_raft_read_proven(const struct efs_raft *r, uint64_t *round,
                          uint64_t *index);

struct efs_raft_store *efs_raft_mem_create(void);
void efs_raft_mem_free(struct efs_raft_store *st);

#endif
