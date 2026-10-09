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
/* Same flush, but BUSY immediately when L0 cannot take another full
 * memtable (one file per key range). Does not walk the memtable and
 * does not wait. The raft pump uses this; a waiting flush there was
 * the snapshot-open stack. */
int efs_kv_lsm_flush_nowait(struct efs_kv *kv);
int efs_kv_lsm_compact(struct efs_kv *kv);
int efs_kv_lsm_seg_count(struct efs_kv *kv, uint32_t *l0, uint32_t *l1);
/* Shape snapshot for the pump's `kv-obs` line (the W23 stalled-compactor
 * test samples it). mt_bytes grows past memtable_max while L0 stays hot
 * (D9); l0_bytes is the D9/D10 byte budget. */
struct efs_kv_lsm_stats {
    uint64_t mt_bytes;
    uint64_t l0_bytes;
    uint32_t n_l0;
    uint32_t n_l1;
};
int efs_kv_lsm_stats(struct efs_kv *kv, struct efs_kv_lsm_stats *out);
/* D30 observation only: never rejects or waits for already committed apply.
 * Thresholds describe this engine, not a process RSS or Raft log bound.
 * soft starts at 3/4 of the unchanged production L0 cap; hard marks deferred
 * flush or an oversized memtable. The admission owner must additionally
 * reserve concurrent/in-flight work and recovery capacity before proposing. */
struct efs_kv_lsm_pressure {
    struct efs_kv_lsm_stats shape;
    uint64_t memtable_target;
    uint64_t l0_soft_bytes;
    uint64_t l0_hard_bytes;
    int soft;
    int hard;
    int io_failed;
};
int efs_kv_lsm_pressure(struct efs_kv *kv, struct efs_kv_lsm_pressure *out);

/* 1 when L0 is within one flush of the file cap. Publish admission
 * returns BUSY; the apply path does not wait. */
int efs_kv_lsm_l0_hot(struct efs_kv *kv);
/* Nested. Hold: WAL write + memtable apply, no fsync. Release (last nest):
 * one fsync. No-op on a non-LSM store. Raft apply of a batched PUBLISH
 * holds across the entry so 256 pubs share one durable boundary. */
int efs_kv_lsm_sync_hold(struct efs_kv *kv);
int efs_kv_lsm_sync_release(struct efs_kv *kv);

/* A pinned view is the segment set at pin time. Flush and compaction
 * publish a new set; reads through the view stay on the old files until
 * the last unpin (that is when the file is unlinked). */
struct efs_kv_lsm_view;
int efs_kv_lsm_view_pin(struct efs_kv *kv, struct efs_kv_lsm_view **out);
int efs_kv_lsm_view_get(struct efs_kv_lsm_view *v, const uint8_t *key,
                        uint32_t klen, uint8_t *val, uint32_t *vlen);
void efs_kv_lsm_view_unpin(struct efs_kv_lsm_view *v);
/* Write this group's live keys to path (atomic rename). The view must
 * have been pinned after a memtable flush. Does not take the LSM lock. */
int efs_kv_lsm_view_export(struct efs_kv_lsm_view *v, uint8_t group,
                           const char *path);
/* Full applied-state snapshot for background metadata/index work. Unlike a
 * segment view, this includes a private copy of the live memtable (including
 * tombstones), captured atomically with the pinned segment set. No flush or
 * WAL write is required. mt_limit bounds memtable copying under the KV lock;
 * BUSY means that budget was exceeded, IO means the engine is poisoned.
 * This is a local KV cut, not a cross-group Raft/transaction watermark.
 * Call from a background worker. Release snapshots promptly: compaction's old
 * segment files remain pinned. scans/gets may run concurrently and callbacks
 * may read the snapshot or mutate the live KV without taking its lock.
 * The caller must join all readers before release. */
struct efs_kv_lsm_snapshot;
int efs_kv_lsm_snapshot_pin(struct efs_kv *kv, uint64_t mt_limit,
                            struct efs_kv_lsm_snapshot **out);
int efs_kv_lsm_snapshot_scan(struct efs_kv_lsm_snapshot *s,
                             const uint8_t *prefix, uint32_t plen,
                             const uint8_t *start, uint32_t slen,
                             int (*cb)(void *, const uint8_t *, uint32_t,
                                       const uint8_t *, uint32_t), void *user);
int efs_kv_lsm_snapshot_get(struct efs_kv_lsm_snapshot *s,
                            const uint8_t *key, uint32_t klen,
                            uint8_t *val, uint32_t *vlen);
void efs_kv_lsm_snapshot_release(struct efs_kv_lsm_snapshot *s);
/* Dirty hints for incremental index refresh. Arrays have 4096 entries. They
 * count applied writes to inode/dentry/lane and transaction metadata families,
 * not user operations. Reset on engine open/recovery; zero is not proof of an
 * empty/rebuilt shard. The index must rebuild once per server boot. Snapshot
 * hints belong to the same local KV cut as its data. Failed/poisoned state is
 * never presented as a fresh snapshot or an advanced index watermark. */
/* Startup-only host policy; enabling requires a fresh full catalog build. */
void efs_kv_lsm_query_tracking(struct efs_kv *kv,int enabled);
int efs_kv_lsm_metadata_revisions(struct efs_kv *kv, uint64_t *out, uint32_t n);
/* Process-local transaction activity fence. Snapshot value is captured under
 * the same KV lock as the metadata image. Saturation refuses comparisons. */
int efs_kv_lsm_transaction_revision(struct efs_kv *kv,uint64_t *out);
int efs_kv_lsm_snapshot_transaction_revision(struct efs_kv_lsm_snapshot *s,uint64_t *out);
int efs_kv_lsm_snapshot_revisions(struct efs_kv_lsm_snapshot *s,
                                  uint64_t *out, uint32_t n);


/* Block until background compaction has caught up (n_l0 below the
 * trigger and no compaction in progress). */
int efs_kv_lsm_quiesce(struct efs_kv *kv);

/* W44 step a: what the CALLING THREAD's prefix scans (merge_scan) did
 * since the last reset — scans run, segment iterators opened, merged
 * keys visited, of which emitted to the callback vs tombstones consumed.
 * Per thread, so the GC thread can print its own numbers on the
 * `gc-pass` line without handler lookups mixed in. */
struct efs_kv_scan_stats {
    uint64_t scans;
    uint64_t segs;
    uint64_t keys;
    uint64_t emitted;
    uint64_t tombstones;
};
void efs_kv_lsm_scan_stats(struct efs_kv_scan_stats *out, int reset);

#endif
