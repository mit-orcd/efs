#ifndef EFS_SIM_H
#define EFS_SIM_H

#include "efs/common.h"
#include "efs/store.h"
#include "efs/opid.h"
#include "efs/txn.h"
#include "efs/lock.h"
#include "efs/meta_apply.h"

/* Deterministic simulator (architecture.md §10 steps 1–10).
 * N logical servers + M clients in one process. Seeded PRNG drives
 * message order, drops, delays, crashes, and clock steps. Metadata is
 * a Raft group (servers 0..2 vote initially; membership changes through
 * joint consensus). A separate control-plane group owns desired placement.
 * Data plane is mem store + loop transport — not sockets or NVMe.
 *
 * Logical chunk size is EFS_SIM_CHUNK (encode takes chunk_size). */

#define EFS_SIM_MAX_SERVERS 8
#define EFS_SIM_MAX_CLIENTS 4
#define EFS_SIM_CHUNK       EFS_MIN_CHUNK_SIZE /* encode floor */
#define EFS_SIM_RAFT_N      3 /* RF = 2f+1, f=1 */
#define EFS_SIM_META        0 /* replica 0 of the metadata Raft group */
#define EFS_SIM_TXN_PREPARE  1
#define EFS_SIM_TXN_DECISION 2
#define EFS_SIM_TXN_RESOLVE  3
#define EFS_SIM_FENCE_BEGIN  1
#define EFS_SIM_FENCE_LOCAL  2
#define EFS_SIM_FENCE_ACK    3
#define EFS_SIM_FENCE_ACTIVE 4

struct efs_sim;

struct efs_sim_cfg {
    uint64_t seed;
    int nservers;           /* >= 3 for 2+1 placement */
    int nclients;
    uint32_t delay_max;     /* extra ticks; 0 = same-tick FIFO */
    uint32_t drop_per_mille; /* 0..1000 chance a scheduled msg is dropped */
    uint64_t export_salt;   /* chosen at mkfs; MKDIR scatter hashes with it */
};

struct efs_sim *efs_sim_new(const struct efs_sim_cfg *cfg);
void efs_sim_free(struct efs_sim *sim);

uint64_t efs_sim_rng(struct efs_sim *sim);
uint64_t efs_sim_now(const struct efs_sim *sim);
uint64_t efs_sim_history(const struct efs_sim *sim);

/* Hold=1 queues events without applying; hold=0 drains. */
void efs_sim_hold(struct efs_sim *sim, int hold);
int efs_sim_drain(struct efs_sim *sim);
int efs_sim_check(struct efs_sim *sim);

/* Namespace ops: propose to the metadata Raft leader, apply on each replica. */
/* mkfs proposes ROOT through Raft (idempotent). Boot already does this. */
int efs_sim_mkfs(struct efs_sim *sim);
int efs_sim_export_salt(struct efs_sim *sim, uint64_t *out);
int efs_sim_create(struct efs_sim *sim, int client, efs_ino_t parent,
                   uint32_t mode, const char *name, efs_ino_t *out);
/* I16: same op identity does not mint a second inode. */
int efs_sim_create_op(struct efs_sim *sim, int client, const struct efs_opid *op,
                      efs_ino_t parent, uint32_t mode, const char *name,
                      efs_ino_t *out);
void efs_sim_opid_for(struct efs_sim *sim, int client, uint64_t seq,
                      struct efs_opid *out);
int efs_sim_opid_ack(struct efs_sim *sim, int client, uint64_t contiguous_ack);
/* Drop the client's RAM window; durable KV is the source of truth (I16). */
int efs_sim_opid_forget(struct efs_sim *sim, int client);
int efs_sim_lookup(struct efs_sim *sim, int client, efs_ino_t parent,
                   const char *name, efs_ino_t *out);
int efs_sim_lookup_path(struct efs_sim *sim, efs_ino_t start, const char *path,
                        struct efs_meta_path_hop *hops, uint32_t cap, uint32_t *n);
int efs_sim_getattr(struct efs_sim *sim, efs_ino_t ino, struct efs_meta_stat *out);
int efs_sim_setattr(struct efs_sim *sim, int client, efs_ino_t ino,
                    const struct efs_meta_setattr *sa);
int efs_sim_utimens(struct efs_sim *sim, int client, efs_ino_t ino,
                    const struct efs_meta_utimens *u);
int efs_sim_truncate(struct efs_sim *sim, int client, efs_ino_t ino,
                     uint64_t size, const struct efs_meta_pub *tail);
int efs_sim_append_reserve(struct efs_sim *sim, int client, efs_ino_t ino,
                           uint64_t len, uint64_t *off_out);
int efs_sim_append_reserve_op(struct efs_sim *sim, int client,
                              const struct efs_opid *op, efs_ino_t ino,
                              uint64_t len, uint64_t *off_out);
int efs_sim_append_resolve(struct efs_sim *sim, int client, efs_ino_t ino,
                           uint64_t off, int outcome);
int efs_sim_readdir(struct efs_sim *sim, efs_ino_t dir,
                    struct efs_meta_dir_cursor *cur, struct efs_meta_dir_ent *out,
                    uint32_t max, uint32_t *n);
int efs_sim_unlink(struct efs_sim *sim, int client, efs_ino_t parent,
                   const char *name);

/* MKDIR is a 2-shard txn (dentry+parent nlink on parent shard, dir row
 * on hash(parent,name,salt)). until: 1=PREPARE 2=DECISION 3=RESOLVE. */
int efs_sim_mkdir(struct efs_sim *sim, int client, efs_ino_t parent,
                  const char *name, efs_ino_t *out);
int efs_sim_mkdir_until(struct efs_sim *sim, efs_ino_t parent, const char *name,
                        struct efs_txid *txid, efs_ino_t *out, int until);
int efs_sim_txn_finish(struct efs_sim *sim, const struct efs_txid *t, int commit);

int efs_sim_link(struct efs_sim *sim, int client, efs_ino_t src_parent,
                 const char *src_name, efs_ino_t dst_parent, const char *dst_name);
int efs_sim_link_until(struct efs_sim *sim, efs_ino_t src_parent,
                       const char *src_name, efs_ino_t dst_parent,
                       const char *dst_name, struct efs_txid *txid, int until);
int efs_sim_rmdir(struct efs_sim *sim, int client, efs_ino_t parent,
                  const char *name);
int efs_sim_rmdir_until(struct efs_sim *sim, efs_ino_t parent, const char *name,
                        struct efs_txid *txid, int until);
int efs_sim_rename(struct efs_sim *sim, int client, efs_ino_t src_parent,
                   const char *src_name, efs_ino_t dst_parent,
                   const char *dst_name);
int efs_sim_rename_until(struct efs_sim *sim, efs_ino_t src_parent,
                         const char *src_name, efs_ino_t dst_parent,
                         const char *dst_name, struct efs_txid *txid, int until);

/* Logical data protocol. skip_frag -1 = PUT all k+f; else omit that index.
 * Fragments are FileID + unique candidate generation. publish is CAS
 * against the committed base (expected 0 if none) plus a lane MAX, and
 * refuses unless I14 holds. read_chunk reconstructs only the *published*
 * candidate; skips corrupt fragments (I25) and never serves unpublished
 * or losing-CAS orphans (I15/I20). inode_generation 0 on put_as / frag_id
 * means the live FileID. */
int efs_sim_put_stripe(struct efs_sim *sim, int client, efs_ino_t ino,
                       uint32_t chunk_index, const uint8_t *chunk,
                       uint32_t chunk_len, int skip_frag);
int efs_sim_put_stripe_as(struct efs_sim *sim, int client, efs_ino_t ino,
                          uint32_t chunk_index, const uint8_t *chunk,
                          uint32_t chunk_len, int skip_frag,
                          uint64_t inode_generation, uint32_t retry);
int efs_sim_publish(struct efs_sim *sim, int client, efs_ino_t ino,
                    uint32_t chunk_index, uint64_t new_size);
int efs_sim_publish_cas(struct efs_sim *sim, int client, efs_ino_t ino,
                        uint32_t chunk_index, uint64_t new_size,
                        uint64_t expected_gen, uint32_t retry,
                        uint64_t content_epoch);
int efs_sim_epoch_fence(struct efs_sim *sim, efs_ino_t ino);
int efs_sim_read_chunk(struct efs_sim *sim, efs_ino_t ino, uint32_t chunk_index,
                       uint8_t *out, uint32_t chunk_len);
int efs_sim_frag_id(struct efs_sim *sim, int client, efs_ino_t ino,
                    uint32_t chunk_index, uint32_t fragment_index,
                    uint64_t inode_generation, uint32_t retry,
                    struct efs_frag_id *out);
int efs_sim_frag_present(struct efs_sim *sim, const struct efs_frag_id *id);

/* I23 revocation barrier. until: BEGIN/LOCAL/ACK/ACTIVE. Fence is
 * explicit — no timeout. Open-unlinked: one lease per (FileID, session). */
int efs_sim_session_fence(struct efs_sim *sim, int client);
int efs_sim_session_fence_until(struct efs_sim *sim, int client, int until);
int efs_sim_open(struct efs_sim *sim, int client, efs_ino_t ino);
int efs_sim_close(struct efs_sim *sim, int client, efs_ino_t ino);
int efs_sim_reclaim(struct efs_sim *sim, efs_ino_t ino);
int efs_sim_inode_nlink(struct efs_sim *sim, efs_ino_t ino, uint32_t *nlink,
                        uint64_t *gen);

/* Directory layout-epoch spread (§7.4). Size trigger lives in apply
 * (`nents > EFS_DIR_SPREAD_MIN`); a LOCAL→SPLITTING flip auto-drains
 * leftovers and FINISHes. These remain the operator/idempotent steps. */
int efs_sim_dir_begin_split(struct efs_sim *sim, efs_ino_t dir);
int efs_sim_dir_migrate(struct efs_sim *sim, efs_ino_t dir);
int efs_sim_dir_finish_hashed(struct efs_sim *sim, efs_ino_t dir);
int efs_sim_dir_layout(struct efs_sim *sim, efs_ino_t dir, uint8_t *layout,
                       uint64_t *epoch);

/* Distributed POSIX locks (§7.6). lockw enqueues (BUSY) on conflict.
 * wake grants FIFO waiters after a release. owner_kind: 1=process, 2=OFD. */
int efs_sim_lock(struct efs_sim *sim, int client, efs_ino_t ino, uint8_t domain,
                 uint8_t type, uint64_t start, uint64_t end, uint8_t owner_kind,
                 uint64_t owner_id);
int efs_sim_lockw(struct efs_sim *sim, int client, efs_ino_t ino, uint8_t domain,
                  uint8_t type, uint64_t start, uint64_t end, uint8_t owner_kind,
                  uint64_t owner_id);
int efs_sim_unlock(struct efs_sim *sim, int client, efs_ino_t ino, uint8_t domain,
                   uint64_t start, uint64_t end, uint8_t owner_kind,
                   uint64_t owner_id);
int efs_sim_lock_wake(struct efs_sim *sim);

int efs_sim_crash(struct efs_sim *sim, int server); /* RAM gone, disk kept */
int efs_sim_restart(struct efs_sim *sim, int server);
int efs_sim_corrupt(struct efs_sim *sim, int server, const struct efs_frag_id *id);
int efs_sim_partition(struct efs_sim *sim, int server, int on);
int efs_sim_clock_step(struct efs_sim *sim, uint64_t delta);

/* Observability for I1–I4. Leader is -1 if none among reachable replicas.
 * tick: server >= 0 ticks that replica; -1 ticks every reachable replica. */
int efs_sim_meta_leader(const struct efs_sim *sim);
int efs_sim_meta_role(const struct efs_sim *sim, int server);
uint64_t efs_sim_meta_term(const struct efs_sim *sim, int server);
uint64_t efs_sim_meta_commit(const struct efs_sim *sim, int server);
int efs_sim_meta_tick(struct efs_sim *sim, int server);
uint32_t efs_sim_meta_voters(const struct efs_sim *sim, int server);
int efs_sim_meta_joint(const struct efs_sim *sim, int server);
int efs_sim_meta2_leader(const struct efs_sim *sim);
uint32_t efs_sim_meta2_voters(const struct efs_sim *sim, int server);
int efs_sim_meta2_joint(const struct efs_sim *sim, int server);

/* Control plane: desired placement. Actual membership follows via joint
 * consensus on each metadata group (I18, L8). */
int efs_sim_ctrl_set_desired(struct efs_sim *sim, uint32_t voters);
uint32_t efs_sim_ctrl_desired(const struct efs_sim *sim);

#endif
