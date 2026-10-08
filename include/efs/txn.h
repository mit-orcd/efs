#ifndef EFS_TXN_H
#define EFS_TXN_H

#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/opid.h"
#include "efs/fence_view.h"

/* Cross-shard transaction SM (architecture.md §7.2 / §10 step 7).
 * Pure: one KV per participant, no sockets. PREPARE is no-wait
 * (conflict → BUSY). Visibility is at the coordinator's durable
 * decision (I17). cannot-establish-authority is EFS_ERR_IO, never
 * absence (I9). Re-prepare / re-decide / re-resolve are idempotent
 * (I16). */

#define EFS_TXN_ID_LEN   16
/* Namespace work can require 64 directory lanes, 64 ancestry shards,
 * and fixed parent/dentry/op-id participants. Preserve every predicate.
 * The one-byte wire count remains unchanged; older binaries must not
 * participate in transactions exceeding their 64-participant capacity. */
#define EFS_TXN_MAX_PART 144
#define EFS_TXN_NAMESPACE_MAX_PART EFS_TXN_MAX_PART
#define EFS_TXN_PARTS_BYTES (1u + 4u * EFS_TXN_MAX_PART)
#define EFS_TXN_VALUE_MAX (8u + 16u * EFS_FENCE_HISTORY_MAX)
#define EFS_TXN_RECORD_MAX (16u + EFS_TXN_PARTS_BYTES + 14u + EFS_TXN_VALUE_MAX)
/* Largest PREPARE: command/kind/txid, participants, key and exact CAS pair. */
#define EFS_TXN_PREPARE_MAX (18u + EFS_TXN_PARTS_BYTES + 2u + EFS_KV_KEY_MAX + 13u + 2u * EFS_TXN_VALUE_MAX)
#define EFS_TXN_ABSENT UINT32_MAX

#define EFS_TXN_EXCL       1
#define EFS_TXN_REDUCE     2 /* lane MAX triple (+ mtime_gen), seq++ */
#define EFS_TXN_GUARD      3
#define EFS_TXN_REDUCE_INO 4 /* inode-row delta: nlink/nents +-, times MAX,
                              * used_shards OR (§7.2 parent-row reductions) */
#define EFS_TXN_REDUCE_ADD 5 /* u64 add (dseq emptiness witness bump) */
#define EFS_TXN_REDUCE_OPID 6 /* op-id window: ack + record one verdict
                               * (I16 for the txn directory ops; the key
                               * is efs_kv_key_opid on a participant) */

/* Exact-byte CAS PREPARE for rows also mutated by unversioned applies.
 * The durable intent is the existing EXCL format. */
#define EFS_TXN_EXCL_VALUE 7
#define EFS_TXN_CONTENT_FENCE 8 /* atomic inode/lane stamp + history pair */
#define EFS_TXN_LANE_BOOTSTRAP 10 /* inode guard + lane authority adoption */
#define EFS_TXN_CONTENT_RESIZE 9 /* coordinated shrink or epoch-preserving extension */

#define EFS_TXN_UNDECIDED 0
#define EFS_TXN_COMMIT    1
#define EFS_TXN_ABORT     2

#define EFS_TXN_PUT 0
#define EFS_TXN_DEL 1

struct efs_txid {
    uint8_t bytes[EFS_TXN_ID_LEN];
};

struct efs_txn_parts {
    uint8_t n;
    uint32_t shard[EFS_TXN_MAX_PART];
};

struct efs_txn_reduce {
    uint64_t max_end;
    uint64_t max_mtime;
    uint64_t max_ctime;
    uint64_t mtime_gen; /* MAX; a dir-lane stamp re-tags the lane after utimens */
};

/* Commutative parent-row update (§7.2). A directory's home row is written
 * by every create/unlink/mkdir/rmdir/link/rename under it, from the
 * same-group log path AND from cross-group transactions. As an EXCL part
 * the txn carried a full row image from its read snapshot and CAS'd the
 * row's version — but the log path writes the row unversioned, so a txn
 * that read before a log-path apply still prepared at the old version,
 * won, and overwrote the apply (IO-500 mdtest, Sep 20: parent nlink=2 with
 * three live subdirectories, every further rmdir EIO). Two txns on the same
 * parent also serialized on that CAS (one STALE, 50 ms × 2ⁿ retry). As a
 * REDUCE the txn carries only what it changes; resolve folds the delta into
 * the row as it is THEN, so it commutes with the log path and with every
 * other reduce, and nothing in one directory conflicts unless it is the
 * same name. */
struct efs_txn_ino_delta {
    int32_t d_nlink;
    int32_t d_nents;          /* LOCAL child count; may flip layout to SPLITTING */
    uint64_t max_mtime;
    uint64_t max_ctime;
    uint64_t or_used_shards;
    efs_ino_t set_parent;     /* 0 = unchanged (rename moves the inode) */
    int32_t d_pver;           /* directory parent_version counter (rename) */
};

/* Wire payload sizes of the PREPARE kinds (after the key). */
#define EFS_TXN_REDUCE_WIRE     32
#define EFS_TXN_REDUCE_INO_WIRE 48
#define EFS_TXN_REDUCE_ADD_WIRE 8
#define EFS_TXN_REDUCE_OPID_WIRE 36 /* seq, rc, ino, extra, ack (BE) */
#define EFS_TXN_GUARD_WIRE      8

/* Coordinator = parts.shard[hash(txid) % n] (§7.2). */
uint32_t efs_txn_hash(const struct efs_txid *t);
uint32_t efs_txn_coordinator(const struct efs_txid *t,
                             const struct efs_txn_parts *p);

int efs_txn_ver_get(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                    uint64_t *ver);

int efs_txn_prepare_excl(struct efs_kv *kv, const struct efs_txid *t,
                         const struct efs_txn_parts *p, const uint8_t *key,
                         uint32_t klen, uint64_t expected_ver, int op,
                         const uint8_t *new_val, uint32_t nlen);
/* expected_len == EFS_TXN_ABSENT means the key must be absent; zero
 * means an existing empty value. Caller serializes on the participant log.
 * A matching outstanding intent is idempotent; a changed replacement or
 * participant list for that intent is STALE. The coordinator
 * must not reuse a completed txid for a different operation. Writers must
 * honor EXCL intents before mutating any protected key. */
int efs_txn_prepare_excl_value(struct efs_kv *kv, const struct efs_txid *t,
                               const struct efs_txn_parts *p, const uint8_t *key,
                               uint32_t klen, const uint8_t *expected_value,
                               uint32_t expected_len, int op,
                               const uint8_t *new_value, uint32_t new_len);
/* Wire: op:u8, expected_len:u32, new_len:u32, expected bytes, new bytes;
 * lengths are big-endian; ABSENT contributes no expected bytes. */
int efs_txn_encode_excl_value(uint8_t *out, uint32_t cap, uint32_t *len,
                              const uint8_t *expected_value,
                              uint32_t expected_len, int op,
                              const uint8_t *new_value, uint32_t new_len);

struct efs_txn_value_cas {
    const uint8_t *key;
    uint32_t klen;
    const uint8_t *expected_value;
    uint32_t expected_len;
    int op;
    const uint8_t *new_value;
    uint32_t new_len;
};
/* One authority's stamp/history pair: at most two distinct keys on the
 * same participant shard. Validation/conflict failure installs neither;
 * storage failure leaves the KV unchanged. Existing matching intents are
 * retained. Caller owns participant-log serialization. */
int efs_txn_prepare_excl_pair(struct efs_kv *kv, const struct efs_txid *t,
                              const struct efs_txn_parts *p,
                              const struct efs_txn_value_cas *requests,
                              uint32_t count);
/* Same atomic validation, up to three distinct keys on one shard. The
 * resize inode authority can include retirement of a closed append cursor. */
int efs_txn_prepare_excl_batch(struct efs_kv *kv, const struct efs_txid *t,
                               const struct efs_txn_parts *p,
                               const struct efs_txn_value_cas *requests,
                               uint32_t count);

int efs_txn_prepare_guard(struct efs_kv *kv, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t observed_ver);
int efs_txn_prepare_reduce(struct efs_kv *kv, const struct efs_txid *t,
                           const struct efs_txn_parts *p, const uint8_t *key,
                           uint32_t klen, const struct efs_txn_reduce *red);
int efs_txn_prepare_ino_delta(struct efs_kv *kv, const struct efs_txid *t,
                              const struct efs_txn_parts *p, const uint8_t *key,
                              uint32_t klen, const struct efs_txn_ino_delta *d);
int efs_txn_prepare_add(struct efs_kv *kv, const struct efs_txid *t,
                        const struct efs_txn_parts *p, const uint8_t *key,
                        uint32_t klen, uint64_t add);
/* Op-id window record (key = efs_kv_key_opid on a participant shard):
 * COMMIT folds `ack` and the verdict for q->id.seq into the window as it
 * is then (efs_opid_fold); ABORT drops the record, nothing committed. */
int efs_txn_prepare_opid(struct efs_kv *kv, const struct efs_txid *t,
                         const struct efs_txn_parts *p, const uint8_t *key,
                         uint32_t klen, const struct efs_opid_req *q,
                         const struct efs_opid_reply *reply);
int efs_txn_drop(struct efs_kv *kv, const struct efs_txid *t, uint32_t shard);

/* BUSY if any transaction holds an EXCL intent or a pending reduce on
 * `key`; EFS_OK when the key is free. The log path calls this before it
 * DELETEs a row (rmdir / last unlink) or otherwise depends on a row no
 * transaction is in the middle of changing — the same rule the txn layer
 * applies to itself ("PREPARE is no-wait, conflict → BUSY"). */
/* Only an EXCL intent blocks ordinary applied writes; namespace REDUCEs
 * intentionally commute with them. Does not consult a remote coordinator. */
int efs_txn_key_exclusive(struct efs_kv *kv, const uint8_t *key, uint32_t klen);

int efs_txn_key_busy(struct efs_kv *kv, const uint8_t *key, uint32_t klen);

/* Current value of a dseq witness (0 when absent). GUARDs on a DSEQ key
 * compare this value, not the version sidecar: the witness is bumped by
 * log-path applies and REDUCE_ADD folds, neither of which is versioned. */
int efs_txn_dseq_observe(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                         uint64_t *seq);

/* Wire encoders for the reduce PREPARE payloads (big-endian; the sizes are
 * EFS_TXN_*_WIRE) and the shared decoder both the server and the simulator
 * apply through, so the format lives in one place. `pay`/`plen` is the
 * PREPARE command after the key. */
void efs_txn_encode_reduce(uint8_t out[EFS_TXN_REDUCE_WIRE],
                           const struct efs_txn_reduce *red);
void efs_txn_encode_ino_delta(uint8_t out[EFS_TXN_REDUCE_INO_WIRE],
                              const struct efs_txn_ino_delta *d);
void efs_txn_encode_add(uint8_t out[EFS_TXN_REDUCE_ADD_WIRE], uint64_t add);
void efs_txn_encode_opid(uint8_t out[EFS_TXN_REDUCE_OPID_WIRE],
                         const struct efs_opid_req *q,
                         const struct efs_opid_reply *reply);
int efs_txn_apply_prepare(struct efs_kv *kv, int kind, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, const uint8_t *pay, uint32_t plen);

int efs_txn_decide(struct efs_kv *kv, uint32_t coord_shard,
                   const struct efs_txid *t, int decision);
int efs_txn_decision_get(struct efs_kv *kv, uint32_t coord_shard,
                         const struct efs_txid *t, int *decision);

/* coord returns EFS_OK and sets *decision, NOT_FOUND for no record
 * (treated as UNDECIDED), or a resource error (I9). A first UNDECIDED
 * is re-checked before the old value is returned. */
typedef int (*efs_txn_coord_fn)(void *ctx, const struct efs_txid *t,
                                uint32_t coord_shard, int *decision);

int efs_txn_read(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                 efs_txn_coord_fn coord, void *ctx, uint8_t *val,
                 uint32_t *vlen);
int efs_txn_reduce_read(struct efs_kv *kv, const uint8_t *lane_key,
                        uint32_t klen, efs_txn_coord_fn coord, void *ctx,
                        struct efs_txn_reduce *out);

/* The transactions a read resolved as UNDECIDED.
 *
 * A validated collect cannot rely on version checks alone. Intents are
 * written at PREPARE, so they are already present in the keys the reader
 * inspects; what moves afterwards is the DECISION, which lives at the
 * coordinator. A reader that resolved lane A's intent as undecided (and
 * excluded it) and then, after the commit, resolved lane B's as committed
 * (and included it) has mixed two states of one atomic transaction — and
 * every version it checks is unchanged, because no intent changed. Only
 * decisions moved.
 *
 * A decision is final, so re-checking just the undecided ones closes it;
 * anything already decided is stable by construction. That keeps the cost
 * proportional to the transactions the read actually met, usually none. */
#define EFS_TXN_MAX_PENDING 16

struct efs_txn_pending {
    uint8_t n;
    /* More undecided transactions than can be tracked. The read cannot be
     * validated, so the caller must retry rather than trust it. */
    uint8_t overflow;
    struct efs_txid txid[EFS_TXN_MAX_PENDING];
    struct efs_txn_parts parts[EFS_TXN_MAX_PENDING];
};

/* EXCL-aware read with the same undecided-set validation as reductions.
 * Accumulate across every key in a collect, then recheck after its final read. */
int efs_txn_read_ex(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                    efs_txn_coord_fn coord, void *ctx, uint8_t *val,
                    uint32_t *vlen, struct efs_txn_pending *pend);

/* As efs_txn_reduce_read, and records the undecided set. `pend` accumulates
 * across calls so one collect over many lanes builds a single set; zero it
 * before the collect, not between lanes. */
int efs_txn_reduce_read_ex(struct efs_kv *kv, const uint8_t *lane_key,
                           uint32_t klen, efs_txn_coord_fn coord, void *ctx,
                           struct efs_txn_reduce *out,
                           struct efs_txn_pending *pend);

/* *moved = 0 iff every recorded txid is STILL undecided, which is what lets
 * a collect claim the values it assembled belong to one instant. Overflow
 * reports moved, because it cannot report otherwise. */
int efs_txn_pending_recheck(const struct efs_txn_pending *pend,
                            efs_txn_coord_fn coord, void *ctx, int *moved);

int efs_txn_resolve(struct efs_kv *kv, const struct efs_txid *t, uint32_t shard,
                    int decision);

/* Stranded-transaction recovery (architecture §7.2, L5: "recovery drives
 * every prepared transaction to COMMIT or ABORT"). The distinct transactions
 * that still hold an INTENT, GUARD or REDUCE record on `shard`, each with
 * the participant set its record carries — enough for a host to locate the
 * coordinator, read (or, for an undecided old one, write ABORT as) the
 * decision, and RESOLVE every participant. Records are transient by design,
 * so this is a short scan of three kind prefixes, never of the shard. */
struct efs_txn_pending_rec {
    struct efs_txid t;
    struct efs_txn_parts parts;
};
int efs_txn_scan_pending(struct efs_kv *kv, uint32_t shard,
                         struct efs_txn_pending_rec *out, uint32_t cap,
                         uint32_t *n);

#endif
