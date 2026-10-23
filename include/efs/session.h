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

/* Staged exact lease/lock cleanup phase. The coordinator must prove completed
 * global revocation before proposing; apply rechecks the durable LOCAL floor.
 * This does not resolve append reservations or certify full session cleanup. */
#define EFS_SESSION_CLEANUP_MAX 8u
#define EFS_SESSION_CLEANUP_KEY_MAX 65u
struct efs_session_cleanup_key {
    uint8_t len;
    uint8_t key[EFS_SESSION_CLEANUP_KEY_MAX];
};
/* Local apply proof only; coordinator must separately prove global revocation. */
int efs_session_cleanup_floor(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
int efs_session_cleanup_select(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
    struct efs_session_cleanup_key keys[EFS_SESSION_CLEANUP_MAX], uint32_t *n);
int efs_session_cleanup_apply(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
    const struct efs_session_cleanup_key *keys, uint32_t n);

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
/* Caller must place global owner and local shard on the same ordered Raft
 * group. Rechecks ACTIVE/epoch and atomically records touched bit plus floor. */
int efs_session_register_establish(struct efs_kv *kv,
    const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,uint32_t shard);
int efs_session_begin_fence(struct efs_kv *kv,
                            const uint8_t uuid[EFS_OPID_UUID_LEN]);
int efs_session_ack_fence(struct efs_kv *kv,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t shard);
/* New production completion is epoch-bound; a delayed coordinator cannot
 * finish a later barrier. The legacy entry point remains for persisted logs. */
int efs_session_finish_fence_epoch(struct efs_kv *,const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch);
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
/* The shard-local half of that proof alone (D31 collector apply): OK only
 * when this shard's durable rejection floor is above old_epoch; BUSY when the
 * row is missing or the floor is not yet raised. The coordinator proves the
 * global barrier separately, from the session group. */
int efs_session_local_revoked(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t old_epoch);

int efs_session_establish(struct efs_kv *kv, uint32_t shard,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
int efs_session_fence_local(struct efs_kv *kv, uint32_t shard,
                            const uint8_t uuid[EFS_OPID_UUID_LEN],
                            uint32_t fence_epoch);
/* The single local session check (I23, K5). EFS_OK only when this shard
 * has established exactly `epoch` for `uuid`. A zero identity, a missing
 * row or a not-yet-established epoch is EFS_ERR_UNREGISTERED; an epoch below
 * the rejection floor or below the established epoch is EFS_ERR_FENCED.
 * There is no unregistered or legacy acceptance. */
int efs_session_accept(struct efs_kv *kv, uint32_t shard,
                       const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);

/* Staged transaction ownership barrier. Prepare before any pending-CREATE
 * effect, on every affected participant, using the SAME txid/participant set.
 * An EXCL exact-value intent keeps local epoch/floor changes behind resolution.
 * Host must stop new owner admission on global FENCING and resolve outstanding
 * transactions before retrying FENCE_LOC/acknowledging the barrier. This helper
 * neither coordinates that drain nor activates production pending CREATE. */
struct efs_txid;
struct efs_txn_parts;
/* Namespace replay/effect transactions use a shared local-session guard.
 * Serialized apply validates epoch (efs_session_accept) before the guard;
 * local epoch writes block on all such guards. */
int efs_session_prepare_namespace(struct efs_kv *,uint32_t,const struct efs_opid *,
    const struct efs_txid *,const struct efs_txn_parts *);
int efs_session_prepare_owner(struct efs_kv *kv, uint32_t shard,
    const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
    const struct efs_txid *txid, const struct efs_txn_parts *parts);

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
 * one KV batch. Server wire commands expose effects and authoritative probes;
 * production FUSE ownership remains staged pending runtime session integration.
 * Legacy edge APIs reject versioned records; never mix protocols per session. */
#define EFS_LEASE_EDGE_OPEN 1u
#define EFS_LEASE_EDGE_CLOSE 2u
#define EFS_LEASE_EDGE_CLEAR_PENDING 4u
/* Staged successor: lease effect, replay floor/result and exact close
 * obligation commit atomically. No whole-file cleanup runs in this apply.
 * OPEN checks the obligation's prepared owner. Production activation requires
 * bounded close processing plus session/PUT lifecycle gates. */
#define EFS_CLOSE_STEP_VALUE 36u
/* Revocation obligation domain; production edge allocator never emits MAX. */
#define EFS_CLOSE_SESSION_SEQ UINT64_MAX
#define EFS_CLOSE_STEP_LOCKS 1u
#define EFS_CLOSE_STEP_RETIRE 2u
int efs_close_step_apply(struct efs_kv *kv, efs_ino_t ino,
    const uint8_t identity[EFS_CLOSE_STEP_VALUE], unsigned phase);
int efs_lease_guarded_edge_deferred(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
    const struct efs_opid_req *q, unsigned flags, uint64_t token);
int efs_lease_guarded_probe_deferred(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
    const struct efs_opid_req *q, unsigned flags, uint64_t token, int *verdict);
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
 * refuses while old lease/lock rows remain; the exact-key cleanup coordinator
 * drains those first. Retirement deletes only one replay window and preserves
 * the durable rejection floor. */
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
