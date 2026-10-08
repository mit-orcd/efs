#ifndef EFS_SESSION_H
#define EFS_SESSION_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/opid.h"

/* Client session + open-lease SM (architecture.md §7.5 / §7.6 / §10 step 8).
 * Pure: one KV, no sockets. Revocation is a barrier (I23): ACTIVE(E) →
 * FENCING(E+1) → FENCE every touched shard → ACTIVE(E+1) only after every
 * ACK. Open-unlinked lifetime is one lease per (FileID, session) (I19, L6).
 * Distributed POSIX locking lives in efs/lock.h (step 10). */

#define EFS_SESSION_BITS     4096
#define EFS_SESSION_BITMAP   512
#define EFS_SESSION_ACTIVE   1
#define EFS_SESSION_FENCING  2
#define EFS_SESSION_VAL      1032
#define EFS_SESS_LOCAL_VAL   8

struct efs_session_rec {
    uint32_t epoch;
    uint8_t state;
    uint8_t touched[EFS_SESSION_BITMAP];
    uint8_t acked[EFS_SESSION_BITMAP];
};

struct efs_sess_local {
    uint32_t established;
    uint32_t reject_below; /* reject request epoch < reject_below */
};

int efs_session_bit_get(const uint8_t *map, uint32_t shard);

int efs_session_create(struct efs_kv *kv, const uint8_t uuid[EFS_OPID_UUID_LEN],
                       uint32_t epoch);
int efs_session_get(struct efs_kv *kv, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    struct efs_session_rec *out);
int efs_session_register(struct efs_kv *kv, const uint8_t uuid[EFS_OPID_UUID_LEN],
                         uint32_t epoch, uint32_t shard);
int efs_session_begin_fence(struct efs_kv *kv,
                            const uint8_t uuid[EFS_OPID_UUID_LEN]);
int efs_session_ack_fence(struct efs_kv *kv,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t shard);
int efs_session_finish_fence(struct efs_kv *kv,
                             const uint8_t uuid[EFS_OPID_UUID_LEN]);
/* OK only after ACTIVE(E+1) for dropping epoch `old_epoch` (I23 barrier). */
int efs_session_barrier_done(struct efs_kv *kv,
                             const uint8_t uuid[EFS_OPID_UUID_LEN],
                             uint32_t old_epoch);

/* Receipt/floor reclamation requires BOTH the completed global barrier and
 * a durable local rejection floor above old_epoch. Caller must obtain
 * authoritative views of the session and target shard; this is not a remote
 * coordinator and must never be used on a replica with only cached state. */
int efs_session_reclaimable(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t old_epoch);

int efs_session_establish(struct efs_kv *kv, uint32_t shard,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
int efs_session_fence_local(struct efs_kv *kv, uint32_t shard,
                            const uint8_t uuid[EFS_OPID_UUID_LEN],
                            uint32_t fence_epoch);
int efs_session_accept(struct efs_kv *kv, uint32_t shard,
                       const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);

/* Staged W94 ownership encoding, not yet activated on the wire. Compose this
 * value into the SAME CREATE KV/transaction batch; never acquire it later.
 * One key remains one (FileID, session) lease. Calls run under shard apply
 * serialization, like the ordinary lease SM. */
#define EFS_LEASE_PENDING_VALUE_LEN 10u
int efs_lease_pending_value(uint64_t token,
                            uint8_t out[EFS_LEASE_PENDING_VALUE_LEN]);
/* Clear only matching pending ownership; release_normal=1 is the guarded
 * final-local-close edge. A mismatched token never deletes another lease. */
int efs_lease_pending_close(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
                            uint64_t token, int release_normal);

/* Staged sequenced edges: callers must serialize local ownership and resolve
 * unknown CREATE outcomes before abandoning a known pending token. There are
 * at most 16 unacknowledged edge verdicts/closed records per session/shard.
 * Every effect, replay verdict and acknowledged tombstone retirement shares
 * one KV batch. No production wire path uses this API yet. Legacy edge APIs
 * reject its versioned records; do not mix protocols for one lease session. */
#define EFS_LEASE_EDGE_OPEN 1u
#define EFS_LEASE_EDGE_CLOSE 2u
#define EFS_LEASE_EDGE_CLEAR_PENDING 4u
int efs_lease_guarded_edge(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          const struct efs_opid_req *q, unsigned flags,
                          uint64_t pending_token);
/* Read-only replay lookup after the caller's authoritative ReadIndex.
 * 1 fills verdict, 0 is not recorded, negative means stale/invalid evidence.
 * Does not acquire ownership, advance acknowledgements or retire anything. */
int efs_lease_guarded_probe(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                           const struct efs_opid_req *q, unsigned flags,
                           uint64_t pending_token, int *verdict);
/* Requires the completed global barrier AND local rejection floor, then
 * removes old ownership before forgetting its replay floor. */
int efs_lease_guarded_retire(struct efs_kv *kv, uint32_t shard,
                            const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);

int efs_lease_open(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                   const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
int efs_lease_close(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
/* 1 = at least one lease, 0 = none, negative = error. */
int efs_lease_any(struct efs_kv *kv, efs_ino_t ino, uint64_t gen);
int efs_lease_drop_session(struct efs_kv *kv, uint32_t shard,
                           const uint8_t uuid[EFS_OPID_UUID_LEN],
                           uint32_t epoch);

#endif
