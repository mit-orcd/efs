#ifndef EFS_LOCK_H
#define EFS_LOCK_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/opid.h"

/* POSIX lock SM (architecture.md §7.6 / §10 step 10). Pure: one KV, no
 * sockets. Grant/release are Raft-applied records. The wait queue is
 * leader memory (simulator), not KV. Two conflict domains: record-lock
 * (classic+OFD fcntl) and flock. */

#define EFS_LOCK_FCNTL 0
#define EFS_LOCK_FLOCK 1
#define EFS_LOCK_SH    1
#define EFS_LOCK_EX    2
#define EFS_LOCK_PROC  1
#define EFS_LOCK_OFD   2
#define EFS_LOCK_CAP   32
#define EFS_LOCK_VAL   1

struct efs_lock_owner {
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    uint8_t kind;
    uint64_t id;
};

struct efs_lock_req {
    efs_ino_t ino;
    uint64_t generation;
    uint8_t domain;
    uint8_t type;
    uint64_t start;
    uint64_t end;
    struct efs_lock_owner owner;
};

struct efs_lock_rec {
    struct efs_lock_req req;
};

int efs_lock_grant(struct efs_kv *kv, const struct efs_lock_req *req);
int efs_lock_release(struct efs_kv *kv, const struct efs_lock_req *req);
int efs_lock_blocked(struct efs_kv *kv, const struct efs_lock_req *req,
                     struct efs_lock_owner *by);
/* F_GETLK: no KV write. On OK, out->type is 0 (F_UNLCK) or the first
 * conflicting record (type/start/end/owner). Same-owner does not
 * conflict. out is cleared first, so it must not alias req. */
int efs_lock_getlk(struct efs_kv *kv, const struct efs_lock_req *req,
                   struct efs_lock_req *out);
int efs_lock_owner_blocks(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          uint8_t domain, const struct efs_lock_owner *owner,
                          uint64_t start, uint64_t end, uint8_t type);
int efs_lock_drop_session(struct efs_kv *kv, uint32_t shard,
                          const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint32_t epoch);
/* Drop every lock record for one (ino, gen) — the last-close release. */
int efs_lock_drop_file(struct efs_kv *kv, efs_ino_t ino, uint64_t gen);
int efs_lock_count(struct efs_kv *kv, efs_ino_t ino, uint64_t gen);

#endif
