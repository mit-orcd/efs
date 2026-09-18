#ifndef EFS_RAFT_DISK_INTERNAL_H
#define EFS_RAFT_DISK_INTERNAL_H

#include "efs/checksum.h"
#include "efs/raft_disk.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* Shared by raft_disk.c (group state, store callbacks) and raft_log.c (the
 * file: framing, group commit, replay, rotation). Not a public header.
 *
 * File: u32 magic · u32 version, then records.
 * Record: u32 crc32(payload) · u32 paylen · payload
 *   payload: u8 type · u32 group · type-specific
 *     HARD:  u64 term · u32 voted_for (as unsigned; -1 means "no vote")
 *     ENTRY: u64 index · u64 term · u32 clen · cmd
 *     TRUNC: u64 from_index
 *     SNAP:  u64 last_index · u64 last_term
 *     CFG:   u32 cfg_old · u32 cfg_new
 * A short or crc-failing record at the END of the file is the crash point and
 * ends replay. One in the MIDDLE is corruption and fails the open loudly: a
 * Raft log that silently starts short can un-commit an acknowledged write. */

#define RAFT_DISK_MAGIC   0x52464C31u /* RFL1 */
#define RAFT_DISK_VERSION 1u
#define RAFT_DISK_HDR_LEN 8u

#define RAFT_REC_HARD  1
#define RAFT_REC_ENTRY 2
#define RAFT_REC_TRUNC 3
#define RAFT_REC_SNAP  4
#define RAFT_REC_CFG   5

/* Enforced at append AND at replay: a record replay would refuse to read must
 * be impossible to commit (the kv_wal lesson). */
#define RAFT_CMD_MAX (8u * 1024u * 1024u)
#define RAFT_REC_MAX (RAFT_CMD_MAX + 64u)

#define RAFT_DISK_PATH_MAX 1024
/* Rotate once the file is both sizeable and mostly dead records. */
#define RAFT_ROTATE_MIN_BYTES (4ull * 1024ull * 1024ull)
#define RAFT_ROTATE_RATIO     3ull

struct raft_log_ent {
    uint64_t term;
    uint32_t clen;
    uint8_t *cmd;
};

/* One Raft group's persistent state. `ops` is first so the store pointer and
 * the ctx pointer are the same thing, as in raft_mem.c. */
struct raft_disk_group {
    struct efs_raft_store ops;
    struct efs_raft_disk *d;
    uint32_t group;
    uint64_t term;
    int32_t voted_for;
    uint64_t snap_idx;
    uint64_t snap_term;
    uint32_t cfg_old;
    uint32_t cfg_new;
    int have_cfg;
    struct raft_log_ent *log; /* log[i] is index snap_idx+1+i */
    uint32_t n;
    uint32_t cap;
};

#define RAFT_DISK_MAX_GROUPS 256

struct efs_raft_disk {
    char dir[RAFT_DISK_PATH_MAX];
    int fd;
    int sync_mode;
    uint64_t bytes;      /* file length */
    uint64_t live_bytes; /* records a rotation would keep */
    uint64_t check_at;   /* recompute live_bytes only past here (amortized) */
    struct raft_disk_group *g[RAFT_DISK_MAX_GROUPS];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t sync_done;  /* highest completed fsync round */
    uint64_t sync_want;  /* highest requested round */
    int syncing;
    int sync_hold;   /* >0: append writes, skip fsync (W3 report batch) */
    int io_failed;
    uint8_t *rec; /* append scratch, RAFT_REC_MAX */
};

/* Callers hold d->mu. Appends one record and, in SYNC mode, returns only once
 * it is durable; concurrent appends across groups share one fsync. */
int raft_log_append(struct efs_raft_disk *d, uint8_t type,
                    struct raft_disk_group *g, uint64_t a, uint64_t b,
                    const uint8_t *cmd, uint32_t clen);
int raft_log_replay(struct efs_raft_disk *d);
int raft_log_rotate_locked(struct efs_raft_disk *d);
void raft_log_maybe_rotate_locked(struct efs_raft_disk *d);

/* raft_disk.c, needed by replay. */
struct raft_disk_group *raft_group_get(struct efs_raft_disk *d, uint32_t group);
int raft_group_reserve(struct raft_disk_group *g);
/* Takes ownership of cmd. Cannot fail: the caller reserved capacity first,
 * which is what lets the store log a record and then install it without a
 * window where the durable log and RAM disagree. */
void raft_group_install(struct raft_disk_group *g, uint64_t index,
                        uint64_t term, uint8_t *cmd, uint32_t clen);
int raft_group_put_entry(struct raft_disk_group *g, uint64_t index,
                         uint64_t term, const uint8_t *cmd, uint32_t clen);
int raft_group_truncate(struct raft_disk_group *g, uint64_t index);
int raft_group_snap(struct raft_disk_group *g, uint64_t last_index,
                    uint64_t last_term);
uint64_t raft_group_live_bytes(const struct raft_disk_group *g);

#endif
