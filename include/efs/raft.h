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
#define EFS_RAFT_MSG_SNAP_REQ 5 /* last_log_* = lastIncluded; entries[0] = blob */
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

struct efs_raft_msg {
    uint8_t type;
    uint8_t group;
    int from;
    int to;
    uint64_t term;
    uint64_t boot_id; /* process incarnation; stale packets drop */
    uint64_t last_log_index;
    uint64_t last_log_term;
    int vote_granted;
    uint64_t prev_index;
    uint64_t prev_term;
    uint64_t leader_commit;
    uint64_t match_index;
    int success;
    uint32_t nentries;
    struct efs_raft_entry entries[EFS_RAFT_AE_MAX]; /* batched AE payloads */
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
};

typedef int (*efs_raft_send_fn)(void *net, const struct efs_raft_msg *msg);
typedef int (*efs_raft_apply_fn)(void *app, uint64_t index, uint64_t term,
                                 const uint8_t *cmd, uint32_t clen);
/* Snapshot blob as of last_included. get mallocs *data (raft frees it).
 * put installs that exact prefix — current SM may be ahead of snap_idx.
 * NULL get → metadata-only SNAP_REQ; NULL put rejects a skip-ahead. */
typedef int (*efs_raft_snap_get_fn)(void *app, uint64_t last_index,
                                    uint8_t **data, uint32_t *len);
typedef int (*efs_raft_snap_put_fn)(void *app, uint64_t last_index,
                                    const uint8_t *data, uint32_t len);

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
    efs_raft_snap_get_fn snap_get;
    efs_raft_snap_put_fn snap_put;
};

struct efs_raft;

struct efs_raft *efs_raft_new(const struct efs_raft_cfg *cfg);
void efs_raft_free(struct efs_raft *r);

int efs_raft_tick(struct efs_raft *r);
int efs_raft_recv(struct efs_raft *r, const struct efs_raft_msg *msg);
int efs_raft_propose(struct efs_raft *r, const uint8_t *cmd, uint32_t clen,
                     uint64_t *index_out);
/* Compact log prefix through last_applied. Captures snap_get (if set) so
 * InstallSnapshot can rebuild a learner that never applied 1..snap_idx.
 * KV already durable through last_applied — no snapshot past that (step 4). */
int efs_raft_snapshot(struct efs_raft *r);
/* Crash restart without dropping the log: KV is already durable through
 * idx, so do not re-apply 1..idx. Does not compact. idx is clamped to
 * [snap_idx, last log]. */
int efs_raft_restore_applied(struct efs_raft *r, uint64_t idx);

int efs_raft_role(const struct efs_raft *r);
int efs_raft_id(const struct efs_raft *r);
uint64_t efs_raft_term(const struct efs_raft *r);
uint64_t efs_raft_commit(const struct efs_raft *r);
uint64_t efs_raft_applied(const struct efs_raft *r);
uint64_t efs_raft_snap_index(const struct efs_raft *r);
int efs_raft_leader(const struct efs_raft *r); /* -1 if unknown */

/* Committed voting set (C_old). During joint this is still C_old. */
uint32_t efs_raft_voters(const struct efs_raft *r);
/* 1 if a joint config is committed (C_new not yet COLD-committed). */
int efs_raft_joint(const struct efs_raft *r);
/* Leader-only. new_voters must be odd, 1..MAX bits. Ids not in C_old are
 * added as learners; returns BUSY until each has match_index >= commit.
 * Always goes through joint consensus — there is no skip-joint path. */
int efs_raft_change(struct efs_raft *r, uint32_t new_voters);
int efs_raft_learner_ready(const struct efs_raft *r, int id);

/* ReadIndex: leader records commitIndex, waits for a majority heartbeat
 * in the current term, then applied >= that index. No clock leases. */
int efs_raft_read_begin(struct efs_raft *r);
int efs_raft_read_ready(const struct efs_raft *r);

struct efs_raft_store *efs_raft_mem_create(void);
void efs_raft_mem_free(struct efs_raft_store *st);

#endif
