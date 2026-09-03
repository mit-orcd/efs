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
 *   last_applied (efs_kv_lsm_flush()), because the snapshot drops the log
 *   prefix that would otherwise replay those commands.
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

#endif
