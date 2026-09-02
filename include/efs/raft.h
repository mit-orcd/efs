#ifndef EFS_RAFT_H
#define EFS_RAFT_H

#include "efs/common.h"

/* Single-shard Raft SM (architecture.md §7.1 / §10 step 4).
 * Pure: no sockets, no threads. The caller delivers messages and ticks.
 * Persistent state is behind efs_raft_store. Apply is a callback into the
 * KV SM. Membership is fixed (I18 is step 6). Clocks are never a
 * correctness input — election timeouts are tick counts the caller sets.
 *
 * I1 election uniqueness · I2 leader completeness · I3 term fencing ·
 * I4 quorum ack (a partitioned leader cannot advance commitIndex). */

#define EFS_RAFT_FOLLOWER  0
#define EFS_RAFT_CANDIDATE 1
#define EFS_RAFT_LEADER    2

#define EFS_RAFT_MAX_PEERS 7 /* RF = 2f+1, f ≤ 3 */

#define EFS_RAFT_MSG_VOTE_REQ 1
#define EFS_RAFT_MSG_VOTE_REP 2
#define EFS_RAFT_MSG_AE_REQ   3
#define EFS_RAFT_MSG_AE_REP   4

struct efs_raft_entry {
    uint64_t term;
    uint32_t clen;
    const uint8_t *cmd;
};

struct efs_raft_msg {
    uint8_t type;
    int from;
    int to;
    uint64_t term;
    uint64_t last_log_index;
    uint64_t last_log_term;
    int vote_granted;
    uint64_t prev_index;
    uint64_t prev_term;
    uint64_t leader_commit;
    uint64_t match_index;
    int success;
    uint32_t nentries;
    struct efs_raft_entry entries[1]; /* at most one payload per RPC */
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
    void (*destroy)(void *ctx);
};

typedef int (*efs_raft_send_fn)(void *net, const struct efs_raft_msg *msg);
typedef int (*efs_raft_apply_fn)(void *app, uint64_t index, uint64_t term,
                                 const uint8_t *cmd, uint32_t clen);

struct efs_raft_cfg {
    int id;
    int n; /* replica count, odd, 1..EFS_RAFT_MAX_PEERS */
    uint32_t election_ticks; /* > heartbeat_ticks */
    uint32_t heartbeat_ticks;
    struct efs_raft_store *store;
    void *store_ctx;
    efs_raft_send_fn send;
    void *net;
    efs_raft_apply_fn apply;
    void *app;
};

struct efs_raft;

struct efs_raft *efs_raft_new(const struct efs_raft_cfg *cfg);
void efs_raft_free(struct efs_raft *r);

int efs_raft_tick(struct efs_raft *r);
int efs_raft_recv(struct efs_raft *r, const struct efs_raft_msg *msg);
int efs_raft_propose(struct efs_raft *r, const uint8_t *cmd, uint32_t clen,
                     uint64_t *index_out);
/* Compact log prefix through last_applied (snapshot metadata only; KV is
 * already the applied store). */
int efs_raft_snapshot(struct efs_raft *r);

int efs_raft_role(const struct efs_raft *r);
int efs_raft_id(const struct efs_raft *r);
uint64_t efs_raft_term(const struct efs_raft *r);
uint64_t efs_raft_commit(const struct efs_raft *r);
uint64_t efs_raft_applied(const struct efs_raft *r);
int efs_raft_leader(const struct efs_raft *r); /* -1 if unknown */

/* ReadIndex: leader records commitIndex, waits for a majority heartbeat
 * in the current term, then applied >= that index. No clock leases. */
int efs_raft_read_begin(struct efs_raft *r);
int efs_raft_read_ready(const struct efs_raft *r);

struct efs_raft_store *efs_raft_mem_create(void);
void efs_raft_mem_free(struct efs_raft_store *st);

#endif
