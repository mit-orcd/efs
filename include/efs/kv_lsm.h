#ifndef EFS_KV_LSM_H
#define EFS_KV_LSM_H

#include "efs/kv.h"
#include "efs/kv_key.h"

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
    /* L0 byte cap; 0 = the production 1 GiB. Not an operator setting:
     * tests size it, and fault builds take EFS_FAULT_L0_BYTES. */
    uint64_t l0_bytes;
};

/* Creates dir if absent, else recovers from it: load segments, replay the
 * WAL tail. A torn trailing record is discarded — it was never ACKed. */
struct efs_kv *efs_kv_lsm_open(const char *dir, const struct efs_kv_lsm_cfg *cfg);
/* Every live segment gets a point-get filter, built in the background. */
void efs_kv_lsm_close(struct efs_kv *kv);
/* Install once before starting readers. Called on a typed stored-value
 * decode failure, possibly under a scan lock: callback must not take KV
 * or host locks or perform blocking I/O (a nonblocking pump notification
 * is allowed). Apply poisons its scope with UNSUPPORTED; serving redirects. */
void efs_kv_lsm_set_decode_error(struct efs_kv *,
    void (*)(void *, const uint8_t *, uint32_t), void *);


/* Bounded maintenance scan: counts merged keys including tombstones.
 * rc1 with resume_len>0 is a work stop; resume is the last consumed key.
 * Restart inclusively and skip that key in the callback. rc0 is exhaustion;
 * callback stops/errors preserve their ordinary cursor and resume_len0.
 * This bounds key work, not wall time or segment initialization. */
int efs_kv_lsm_scan_work(struct efs_kv *,const uint8_t *,uint32_t,
    const uint8_t *,uint32_t,uint32_t,
    int (*)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *,
    uint8_t [EFS_KV_KEY_MAX],uint32_t *);

/* Background hint scan, NOT a snapshot: release the KV read lock after each
 * max_keys merged keys (including tombstones), then yield to writers. Rows
 * inserted behind the cursor can wait for the next maintenance pass. Callers
 * must revalidate mutations in serialized apply; never use for read authority
 * or to infer absence. Callback stop/error codes are returned unchanged. */
int efs_kv_lsm_scan_yield(struct efs_kv *, const uint8_t *, uint32_t, uint32_t,
    int (*)(void *, const uint8_t *, uint32_t, const uint8_t *, uint32_t), void *);

/* Minimum-reader barrier (W125). Before the first format-dependent row
 * (a packed segment or packed chunk) becomes visible, the store durably
 * requires PACK_V2, writing a KVLSM2 MANIFEST that builds without packing
 * refuse to open. Requirements only increase. Capability 1 was the archived
 * packing format and is refused, as is any other KVLSM2 requirement. Only
 * success establishes durability; retry after an uncertain failure. An
 * in-memory KV has no on-disk format and always succeeds. */
#define EFS_KV_READER_PACK_V2 2u
int efs_kv_lsm_require_reader(struct efs_kv *, uint32_t required);

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
    /* K22/W23 bound: the L0 byte cap and the memory budget derived from
     * it, min(L0 cap / 8, physical memory / 16), at least 4 memtables.
     * mt_bytes (memtable + insertion buffer = unflushed WAL tail) is
     * held at mem_budget by efs_kv_lsm_apply_room. */
    uint64_t l0_cap;
    uint64_t mem_budget;
    uint64_t apply_deferred; /* apply_room refusals since open */
    uint64_t flushes, compactions; /* D4: completed since open (maintenance progress) */
};
int efs_kv_lsm_stats(struct efs_kv *kv, struct efs_kv_lsm_stats *out);

/* 1 when L0 is within one flush of the file cap. Publish admission
 * returns BUSY; the apply path does not wait. */
int efs_kv_lsm_l0_hot(struct efs_kv *kv);
/* K22/W23: room for one more state-machine apply. EFS_ERR_BUSY while the
 * unflushed memtable/WAL tail is at mem_budget (flushes are deferred because
 * L0 is at its cap, e.g. a stalled compactor); it kicks the flush or the
 * compactor and the caller retries the same entry later, never skipping it.
 * admit: 1 = admission watermark (3/4 of the budget) for new proposals. */
int efs_kv_lsm_apply_room(struct efs_kv *kv, int admit);
/* Same derivation, for a caller sizing other bounds before/without a KV. */
uint64_t efs_kv_lsm_mem_budget(uint64_t l0_cap, uint32_t memtable_max);
/* Nested. Hold: WAL write + memtable apply, no fsync. Release (last nest):
 * one fsync. No-op on a non-LSM store. Raft apply of a batched PUBLISH
 * holds across the entry so 256 pubs share one durable boundary. */
int efs_kv_lsm_sync_hold(struct efs_kv *kv);
int efs_kv_lsm_sync_release(struct efs_kv *kv);
/* Detach exactly one outer hold without claiming durability. A successful
 * sync_prefix is required before publishing its captured recovery cursor.
 * Nested holds return BUSY unchanged, permitting synchronous fallback. */
int efs_kv_lsm_sync_detach(struct efs_kv *kv,uint64_t *prefix);
int efs_kv_lsm_sync_prefix(struct efs_kv *kv,uint64_t prefix);
/* Nonblocking probe of the actual durable WAL frontier. Unknown/future
 * prefixes and sticky storage failures never produce covered=true. */
int efs_kv_lsm_sync_covered(struct efs_kv *kv,uint64_t prefix,int *covered);

/* One bounded Raft entry: reads/scans see this thread's staged changes;
 * other readers see the old committed table. Writers are excluded until end.
 * End commits all effects and the supplied cursor in one atomic WAL record,
 * under the existing sync hold. NULL cursor aborts; no staged effects escape.
 * At most 32,768 effect keys / 64 MiB plus one cursor, independent of filesystem size.
 * Non-LSM stores return INVAL; begin/end must run on the same thread. */
int efs_kv_lsm_apply_begin(struct efs_kv *kv);
int efs_kv_lsm_apply_end(struct efs_kv *kv, const struct efs_kv_item *cursor);

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
 * WAL write is required. mt_limit bounds the snapshot object, copied memtable
 * and retained segment/pin arrays under the KV lock;
 * BUSY means that budget was exceeded, IO means the engine is poisoned.
 * This is a local KV cut, not a cross-group Raft/transaction watermark.
 * Call from a background worker. Release snapshots promptly: compaction's old
 * segment files remain pinned. scans/gets may run concurrently and callbacks
 * may read the snapshot or mutate the live KV without taking its lock.
 * The caller must join all readers before release. */
struct efs_kv_lsm_snapshot;
int efs_kv_lsm_snapshot_pin(struct efs_kv *kv, uint64_t mt_limit,
                            struct efs_kv_lsm_snapshot **out);
/* Private source scans admit at most 256 relevant segments and 128 KiB per
 * block (under 33 MiB iterator/merge scratch). Wider/oversized inputs return
 * BUSY; authoritative live reads/writes retain their existing limits. */
int efs_kv_lsm_snapshot_scan(struct efs_kv_lsm_snapshot *s,
                             const uint8_t *prefix, uint32_t plen,
                             const uint8_t *start, uint32_t slen,
                             int (*cb)(void *, const uint8_t *, uint32_t,
                                       const uint8_t *, uint32_t), void *user);
int efs_kv_lsm_snapshot_get(struct efs_kv_lsm_snapshot *s,
                            const uint8_t *key, uint32_t klen,
                            uint8_t *val, uint32_t *vlen);
void efs_kv_lsm_snapshot_release(struct efs_kv_lsm_snapshot *s);
/* Retained snapshot allocations; excludes borrowed engine segments and
 * temporary per-reader scan iterators, whose limits are reported separately. */
uint64_t efs_kv_lsm_snapshot_bytes(const struct efs_kv_lsm_snapshot *s);
/* Dirty hints for incremental index refresh. Arrays have 4096 entries. They
 * count applied writes to inode/dentry/lane and transaction metadata families,
 * not user operations. Reset on engine open/recovery; zero is not proof of an
 * empty/rebuilt shard. The index must rebuild once per server boot. Snapshot
 * hints belong to the same local KV cut as its data. Failed/poisoned state is
 * never presented as a fresh snapshot or an advanced index watermark. */
/* Startup-only host policy; enabling requires a fresh full catalog build. */
void efs_kv_lsm_query_tracking(struct efs_kv *kv,int enabled);
int efs_kv_lsm_metadata_revisions(struct efs_kv *kv, uint64_t *out, uint32_t n);
/* Dead-inode cleanup scope for the calling thread (nestable; 1 enters,
 * 0 leaves). Inside it, deletes of INODE, LANE and APPEND_CUR rows still
 * advance the shard's query revision, so the next catalog build rebuilds
 * that shard exactly, but not the metadata revision above: removing rows of
 * an inode that no name references never schedules a refresh by itself.
 * Puts and every other family count as before. Process-local and
 * saturating, efs_kv_lsm_cleanup_deferred counts the deferred deletes. */
void efs_kv_lsm_cleanup_scope(int enter);
uint64_t efs_kv_lsm_cleanup_deferred(struct efs_kv *kv);
/* Process-local transaction activity fence. Snapshot value is captured under
 * the same KV lock as the metadata image. Saturation refuses comparisons. */
int efs_kv_lsm_transaction_revision(struct efs_kv *kv,uint64_t *out);
int efs_kv_lsm_snapshot_transaction_revision(struct efs_kv_lsm_snapshot *s,uint64_t *out);
/* Diagnostic hints only, under the same lock/cut as transaction revision.
 * Eight saturating counters: INTENT/GUARD/REDUCE/DECISION, PUT then DEL.
 * Never use these process-local counts as authority or participant coverage. */
int efs_kv_lsm_transaction_activity(struct efs_kv *kv,uint64_t out[8]);
int efs_kv_lsm_snapshot_transaction_activity(struct efs_kv_lsm_snapshot *s,uint64_t out[8]);
/* Durable partition change checkpoints, paired with the exact KV manifest.
 * WAL replay may conservatively advance counters twice after an interrupted
 * reset. Compare only inside the same lineage; missing/corrupt sidecars create
 * a new lineage and force a derived rebuild. Tracking remains active while
 * command serving is disabled. These are not cross-group authority seals. */
int efs_kv_lsm_snapshot_query_revisions(struct efs_kv_lsm_snapshot *s,
                                       uint64_t lineage[2],uint64_t revisions[4096]);
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
