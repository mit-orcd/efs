#ifndef EFS_TXN_H
#define EFS_TXN_H

#include "efs/common.h"
#include "efs/kv.h"

/* Cross-shard transaction SM (architecture.md §7.2 / §10 step 7).
 * Pure: one KV per participant, no sockets. PREPARE is no-wait
 * (conflict → BUSY). Visibility is at the coordinator's durable
 * decision (I17). cannot-establish-authority is EFS_ERR_IO, never
 * absence (I9). Re-prepare / re-decide / re-resolve are idempotent
 * (I16). */

#define EFS_TXN_ID_LEN   16
#define EFS_TXN_MAX_PART 8

#define EFS_TXN_EXCL   1
#define EFS_TXN_REDUCE 2
#define EFS_TXN_GUARD  3

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
};

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
int efs_txn_prepare_guard(struct efs_kv *kv, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t observed_ver);
int efs_txn_prepare_reduce(struct efs_kv *kv, const struct efs_txid *t,
                           const struct efs_txn_parts *p, const uint8_t *key,
                           uint32_t klen, const struct efs_txn_reduce *red);
int efs_txn_drop(struct efs_kv *kv, const struct efs_txid *t, uint32_t shard);

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

#endif
