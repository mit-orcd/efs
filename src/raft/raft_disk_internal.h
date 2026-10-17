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
 *            [· u64 log_base]  (present when the record is 24 bytes of
 *            payload past the type; absent on logs written before the
 *            retained window. log_base is the index before the first
 *            kept entry.)
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

/* Framed lengths of the fixed records a rotation writes. */
#define REC_LEN_HARD      25u
#define REC_LEN_SNAP_BASE 37u
#define REC_LEN_CFG       21u

/* raft_group_snap base_hint for "no retained-window base": a live
 * save_snap and the replay of a pre-window 16-byte SNAP record. A
 * rotated 24-byte record carries the real log_base, which is 0 while
 * the first window is still whole — 0 is a value, not the absence. */
#define RAFT_SNAP_NO_HINT UINT64_MAX

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
    uint64_t end_off; /* file offset after this record; 0 = replayed */
    uint64_t payload_off; /* log position, distinct from durable end_off */
    uint32_t clen;
    uint32_t record_crc; /* immutable entry frame checksum; no disk format change */
    uint8_t *cmd;
    /* Payload position in a rotation image: gen << RAFT_IMG_POS_BITS |
     * (physical offset + 1). Valid only while gen == d->gen; 0 = none.
     * Lets a swap relocate every imaged entry without walking them. */
    uint64_t img_off;
};

/* Offsets are logical: physical + d->base, which never moves backwards
 * across a rotation, so a watermark read before a swap can never cover a
 * record written after it. payload_off with RAFT_OFF_IMAGE set is instead
 * a physical offset inside the current file's image (set when a rotation
 * attempt reads an entry that the previous rotation imaged). */
#define RAFT_OFF_IMAGE     (1ull << 63)
#define RAFT_IMG_POS_BITS  40
#define RAFT_IMG_POS_MASK  ((1ull << RAFT_IMG_POS_BITS) - 1)
#define RAFT_IMG_GEN_MASK  0xFFFFFFu

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
    /* log[i] is index log_base+1+i. Equals snap_idx when nothing before
     * the snapshot is retained. A window keeps log_base < snap_idx. */
    uint64_t log_base;
    /* Running sums of clen: every entry in log[], and the ones with
     * index <= snap_idx (the retained window). new bytes = all - old.
     * Kept here so the pump's snapshot check is O(1). */
    uint64_t bytes_all;
    uint64_t bytes_old;
    uint32_t cfg_old;
    uint32_t cfg_new;
    int have_cfg;
    struct raft_log_ent *log; /* log[i] is index log_base+1+i */
    uint32_t n;
    uint32_t cap;
    uint64_t cache_bytes,cache_cursor;
    int index_fd; /* unlinked, derived index backing; never recovery authority */
    uint64_t index_advised;
    /* Lowest index overwritten or truncated since a rotation captured the
     * group. A rotation still reading that range must start over. */
    uint64_t rot_low;
};

#define RAFT_COMMAND_CACHE_BYTES (2u << 20)
#define RAFT_INDEX_RESIDENT_BYTES (2u << 20)
#define RAFT_DISK_MAX_GROUPS 256

struct efs_raft_disk {
    char dir[RAFT_DISK_PATH_MAX];
    int fd;
    int sync_mode;
    uint64_t bytes;      /* end of valid records, excluding reserved zero tail */
    uint64_t allocated_bytes; /* physical file length, possibly preallocated */
    int prealloc_off;    /* unsupported/full filesystem: ordinary append */
    uint64_t live_bytes; /* records a rotation would keep */
    uint64_t check_at;   /* recompute live_bytes only past here (amortized) */
    struct raft_disk_group *g[RAFT_DISK_MAX_GROUPS];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t sync_done;  /* highest completed fsync round */
    uint64_t sync_want;  /* highest requested round */
    int syncing;
    int sync_hold;   /* >0: append writes, skip fsync (W3 report batch) */
    int sync_need;   /* an append happened during a hold; release fsyncs */
    uint64_t synced_bytes; /* file offset covered by the last fsync */
    /* Diagnostics only, under mu; never affect durable watermarks. */
    uint64_t obs_entries, obs_synced_entries, obs_hold_started;
    unsigned obs_waiters;
    int io_failed;
    uint8_t *rec; /* append scratch, RAFT_REC_MAX */
    /* Rotation runs off the caller's locks (see raft_log.c). base maps
     * physical to logical offsets; gen names the current file's image. */
    uint64_t base;
    uint32_t gen, gen_seq;
    int rotating, rot_want, rot_stop, rot_thread_on;
    /* Store mutators between logging a record and applying it to RAM.
     * raft_log_append can drop d->mu inside that window (fdatasync), and
     * a rotation capture there would find the record in the file but not
     * in RAM. rot_capturing holds new mutators off while capture drains. */
    unsigned applying;
    int rot_capturing;
    pthread_t rot_thread;
    pthread_cond_t rot_cv;
    uint64_t rotations;
    uint64_t rot_hold_max_us; /* longest d->mu hold by the last rotation */
    uint64_t rot_final_us;    /* its final swap, under d->mu */
};

/* Bracket a store mutation (log append + RAM apply). d->mu held. */
static inline void raft_mut_begin(struct efs_raft_disk *d)
{
    while (d->rot_capturing)
        pthread_cond_wait(&d->cv, &d->mu);
    d->applying++;
}

static inline void raft_mut_end(struct efs_raft_disk *d)
{
    if (--d->applying == 0 && d->rot_capturing)
        pthread_cond_broadcast(&d->cv);
}

/* Physical payload offset of e in the current file (d->fd). d->mu held. */
static inline uint64_t raft_ent_phys(const struct efs_raft_disk *d,
                                     const struct raft_log_ent *e)
{
    if (e->img_off && (uint32_t)(e->img_off >> RAFT_IMG_POS_BITS) == d->gen)
        return (e->img_off & RAFT_IMG_POS_MASK) - 1;
    if (e->payload_off & RAFT_OFF_IMAGE)
        return e->payload_off & ~RAFT_OFF_IMAGE;
    return e->payload_off - d->base;
}

/* Callers hold d->mu. Appends one record and, in SYNC mode, returns only once
 * it is durable; concurrent appends across groups share one fsync. */
int raft_log_append(struct efs_raft_disk *d, uint8_t type,
                    struct raft_disk_group *g, uint64_t a, uint64_t b,
                    const uint8_t *cmd, uint32_t clen, uint32_t *record_crc,uint64_t *record_end);
/* This thread's appends skip the fsync. defer_end fsyncs them once,
 * even if other threads still hold the shared sync. Nested. */
void raft_log_defer_begin(void);
int raft_log_defer_end(struct efs_raft_disk *d);
int raft_log_replay(struct efs_raft_disk *d);
int raft_log_read_command(struct raft_disk_group *,uint32_t,int,uint8_t *);
void raft_group_index_trim(struct raft_disk_group *);
int raft_log_rotate_locked(struct efs_raft_disk *d);
void raft_log_maybe_rotate_locked(struct efs_raft_disk *d);
int raft_log_rotator_start(struct efs_raft_disk *d);
void raft_log_rotator_stop(struct efs_raft_disk *d);
/* Tests only: called unlocked between a rotation's copy steps. */
extern void (*raft_log_rotate_hook)(struct efs_raft_disk *d, int step);

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
                    uint64_t last_term, uint64_t base_hint);
uint64_t raft_group_live_bytes(const struct raft_disk_group *g);

#endif
