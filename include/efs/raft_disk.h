#ifndef EFS_RAFT_DISK_H
#define EFS_RAFT_DISK_H

#include "efs/raft.h"

/* On-disk efs_raft_store (architecture.md §10 step 10.5b).
 *
 * The Raft log is the system's durability boundary: an entry that a majority
 * has here is committed, and the applied KV is a materialized view that can
 * be rebuilt by replaying forward from the snapshot point. efs_raft_new()
 * already assumes this — it starts with last_applied = snap_idx and re-applies
 * everything above it — so the ONE rule a caller owes is:
 *
 *   never call efs_raft_snapshot() until the applied KV is durable through
 *   last_applied (efs_kv_lsm_flush()). The snapshot drops the log prefix
 *   outside the retained window (EFS_RAFT_SNAP_BYTES); a follower still
 *   inside that window is served from the log.
 *
 * Physical shape: one multiplexed record log per NODE, not per group. Every
 * group on the node appends to it and concurrent appends share one fsync, so
 * thousands of groups cost one fsync stream instead of thousands. Records
 * carry their group id; a group's state is reconstructed by replay.
 *
 * RAM holds the live (un-snapshotted) log entries, which the snapshot point
 * bounds; the file is the durability boundary and the recovery source.
 *
 * Threading: concurrent use across DIFFERENT groups is supported and is the
 * point of the shared log. One group's store must be driven by one caller at
 * a time, which is already true of efs_raft itself (the state machine is not
 * thread-safe either — see raft.h). */

#define EFS_RAFT_DISK_NOSYNC 0 /* tests and the simulator */
#define EFS_RAFT_DISK_SYNC   1 /* production: fsync before returning */

struct efs_raft_disk;

/* Creates dir (and parents) if absent, then replays any existing log.
 * Returns NULL on a corrupt log rather than silently starting empty. */
struct efs_raft_disk *efs_raft_disk_open(const char *dir, int sync_mode);
void efs_raft_disk_close(struct efs_raft_disk *d);

/* The store for one group, created on first use. Pass the result as BOTH
 * cfg.store and cfg.store_ctx. Owned by the disk: it stays valid across
 * efs_raft_free() (a restarting replica must find its log) and is released
 * by efs_raft_disk_close(). Never pass it to efs_raft_mem_free(). */
struct efs_raft_store *efs_raft_disk_group(struct efs_raft_disk *d,
                                           uint32_t group);

/* Rewrites the log with only live records, dropping what snapshots and
 * truncations made dead. Called automatically as the log grows; exposed for
 * tests. */
int efs_raft_disk_rotate(struct efs_raft_disk *d);
/* Bytes currently in the log file (tests assert rotation actually shrinks). */
uint64_t efs_raft_disk_bytes(const struct efs_raft_disk *d);

/* Defer log fsync across a burst of appends. Nested. The release that
 * drops the count to zero fsyncs once, covering every append during the
 * hold. A hold with no append does not fsync. Crash during hold loses
 * those appends — the caller must not have ACKed them. */
int efs_raft_disk_sync_hold(struct efs_raft_disk *d);
int efs_raft_disk_sync_release(struct efs_raft_disk *d);
int efs_raft_disk_sync_release_wait(struct efs_raft_disk *d);
int efs_raft_disk_sync_depth(struct efs_raft_disk *d);
/* File offset covered by the last completed fsync. */
uint64_t efs_raft_disk_synced_bytes(struct efs_raft_disk *d);
/* Highest log index of `group` whose record ends at or before `synced`.
 * A replayed entry (no offset) counts as covered. 0 if the group is empty. */
uint64_t efs_raft_disk_covered_index(struct efs_raft_disk *d, uint32_t group,
                                    uint64_t synced);

#endif
