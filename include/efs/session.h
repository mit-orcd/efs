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

int efs_session_establish(struct efs_kv *kv, uint32_t shard,
                          const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch);
int efs_session_fence_local(struct efs_kv *kv, uint32_t shard,
                            const uint8_t uuid[EFS_OPID_UUID_LEN],
                            uint32_t fence_epoch);
int efs_session_accept(struct efs_kv *kv, uint32_t shard,
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
