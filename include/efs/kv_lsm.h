#ifndef EFS_KV_LSM_H
#define EFS_KV_LSM_H

#include "efs/kv.h"

/* Durable ordered KV behind efs_kv_ops (architecture.md §10 step 10.5a).
 *
 * WAL + immutable sorted segments + compaction. ONE engine per node: the
 * 12-bit shard prefix in every key (kv_key.h) multiplexes all Raft groups
 * into this store, so its WAL is the shared group-committed log — 4096
 * logical groups, never 4096 physical WALs (arch/performance.md §5.4).
 *
 * RAM held is the memtable plus one sparse index entry per segment block,
 * so it is bounded by write rate and segment count, not by key count.
 *
 * Thread-safe: concurrent put/get/del/scan/batch are serialized internally,
 * and concurrent batches share one fsync. This differs from efs_kv_mem,
 * which is single-threaded. A scan callback may read this same store; it
 * must not write to it (that would mutate what the scan is iterating).
 *
 * A batch is atomic and durable when it returns: its WAL record is fsynced
 * before any item becomes visible, so a failed batch leaves no trace. */

#define EFS_KV_LSM_SYNC   0 /* fsync every batch (production) */
#define EFS_KV_LSM_NOSYNC 1 /* write the WAL, skip fsync (simulator) */

struct efs_kv_lsm_cfg {
    int sync_mode;         /* EFS_KV_LSM_SYNC or EFS_KV_LSM_NOSYNC */
    uint32_t memtable_max; /* bytes before flush; 0 = default */
    uint32_t l0_max;       /* L0 segments before compaction; 0 = default */
};

/* Creates dir if absent, else recovers from it: load segments, replay the
 * WAL tail. A torn trailing record is discarded — it was never ACKed. */
struct efs_kv *efs_kv_lsm_open(const char *dir, const struct efs_kv_lsm_cfg *cfg);
void efs_kv_lsm_close(struct efs_kv *kv);

/* Test/maintenance hooks. flush makes the memtable an L0 segment; compact
 * merges L0 plus the overlapping L1 into a new L1. */
int efs_kv_lsm_flush(struct efs_kv *kv);
int efs_kv_lsm_compact(struct efs_kv *kv);
int efs_kv_lsm_seg_count(struct efs_kv *kv, uint32_t *l0, uint32_t *l1);

#endif
