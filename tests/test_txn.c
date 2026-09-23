/* Isolated transaction SM. No sockets, no Raft. */
#include "efs/txn.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static void tid(struct efs_txid *t, uint8_t n)
{
    memset(t, 0, sizeof(*t));
    t->bytes[0] = n;
}

static struct efs_txn_parts two(uint32_t a, uint32_t b)
{
    struct efs_txn_parts p;

    memset(&p, 0, sizeof(p));
    if (a == b) {
        p.n = 1;
        p.shard[0] = a;
        return p;
    }
    p.n = 2;
    p.shard[0] = a < b ? a : b;
    p.shard[1] = a < b ? b : a;
    return p;
}

struct kvs {
    struct efs_kv *kv;
    int fail;
};

static int coord_ok(void *user, const struct efs_txid *t, uint32_t shard,
                    int *dec)
{
    struct kvs *k = user;

    if (k->fail)
        return EFS_ERR_IO;
    return efs_txn_decision_get(k->kv, shard, t, dec);
}

static void test_excl_conflict_i16(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a, b;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX], val[4] = { 1, 2, 3, 4 };
    uint32_t klen = 0;

    tid(&a, 1);
    tid(&b, 2);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_dentry(1, EFS_ROOT_INO, "x", key, &klen) == EFS_OK, "key");
    CHECK(efs_txn_prepare_excl(kv, &a, &p, key, klen, 0, EFS_TXN_PUT, val, 4) ==
              EFS_OK,
          "prep a");
    CHECK(efs_txn_prepare_excl(kv, &a, &p, key, klen, 0, EFS_TXN_PUT, val, 4) ==
              EFS_OK,
          "I16 re-prep");
    CHECK(efs_txn_prepare_excl(kv, &b, &p, key, klen, 0, EFS_TXN_PUT, val, 4) ==
              EFS_ERR_BUSY,
          "no-wait conflict");
    efs_kv_mem_free(kv);
}

static void test_i17_visible_at_decision(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct kvs ctx;
    struct efs_txid t;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX], val[4] = { 9, 9, 9, 9 }, got[8];
    uint32_t klen = 0, n;
    uint32_t coord;

    tid(&t, 3);
    ctx.kv = kv;
    ctx.fail = 0;
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_dentry(1, EFS_ROOT_INO, "y", key, &klen) == EFS_OK, "key");
    CHECK(efs_txn_prepare_excl(kv, &t, &p, key, klen, 0, EFS_TXN_PUT, val, 4) ==
              EFS_OK,
          "prep");
    n = sizeof(got);
    CHECK(efs_txn_read(kv, key, klen, coord_ok, &ctx, got, &n) ==
              EFS_ERR_NOT_FOUND,
          "NO-DECISION → old absent");
    coord = efs_txn_coordinator(&t, &p);
    CHECK(efs_txn_decide(kv, coord, &t, EFS_TXN_COMMIT) == EFS_OK, "decide");
    n = sizeof(got);
    CHECK(efs_txn_read(kv, key, klen, coord_ok, &ctx, got, &n) == EFS_OK &&
              n == 4 && memcmp(got, val, 4) == 0,
          "COMMIT visible before resolve");
    CHECK(efs_txn_resolve(kv, &t, 1, EFS_TXN_COMMIT) == EFS_OK, "resolve");
    n = sizeof(got);
    CHECK(efs_kv_get(kv, key, klen, got, &n) == EFS_OK && n == 4, "materialized");
    efs_kv_mem_free(kv);
}

static void test_i9(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct kvs ctx;
    struct efs_txid t;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX], val[2] = { 1, 2 }, got[8];
    uint32_t klen = 0, n;

    tid(&t, 4);
    ctx.kv = kv;
    ctx.fail = 1;
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_dentry(1, EFS_ROOT_INO, "z", key, &klen) == EFS_OK, "key");
    CHECK(efs_txn_prepare_excl(kv, &t, &p, key, klen, 0, EFS_TXN_PUT, val, 2) ==
              EFS_OK,
          "prep");
    n = sizeof(got);
    CHECK(efs_txn_read(kv, key, klen, coord_ok, &ctx, got, &n) == EFS_ERR_IO,
          "I9 never absent");
    efs_kv_mem_free(kv);
}

static void test_reduce_no_block(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct kvs ctx;
    struct efs_txid a, b;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0;
    struct efs_txn_reduce ra = { .max_end = 100 }, rb = { .max_end = 500 };
    struct efs_txn_reduce got;
    uint32_t coord_a, coord_b;

    tid(&a, 5);
    tid(&b, 6);
    ctx.kv = kv;
    ctx.fail = 0;
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_lane(1, 1, 1, 0, key, &klen) == EFS_OK, "lane");
    CHECK(efs_txn_prepare_reduce(kv, &a, &p, key, klen, &ra) == EFS_OK, "ra");
    CHECK(efs_txn_prepare_reduce(kv, &b, &p, key, klen, &rb) == EFS_OK,
          "rb no-block");
    coord_a = efs_txn_coordinator(&a, &p);
    coord_b = efs_txn_coordinator(&b, &p);
    CHECK(efs_txn_decide(kv, coord_a, &a, EFS_TXN_COMMIT) == EFS_OK, "da");
    CHECK(efs_txn_decide(kv, coord_b, &b, EFS_TXN_COMMIT) == EFS_OK, "db");
    CHECK(efs_txn_reduce_read(kv, key, klen, coord_ok, &ctx, &got) == EFS_OK &&
              got.max_end == 500,
          "MAX pending");

    /* A materialized lane record is longer than the reduce triple, because
     * its owner keeps lane_seq and its fences after it. Reading one must
     * work: a lane that has ever been published is the normal case, and the
     * pending-only case above is the rare one. */
    {
        uint8_t lv[64];
        memset(lv, 0, sizeof(lv));
        lv[7] = 200;                  /* max_end = 200, big-endian */
        lv[15] = 7;                   /* max_mtime = 7 */
        lv[23] = 9;                   /* max_ctime = 9 */
        lv[31] = 3;                   /* lane_seq = 3, past the triple */
        CHECK(efs_kv_put(kv, key, klen, lv, sizeof(lv)) == EFS_OK, "lane put");
        CHECK(efs_txn_reduce_read(kv, key, klen, coord_ok, &ctx, &got) == EFS_OK,
              "materialized lane readable");
        CHECK(got.max_end == 500, "committed reduction still dominates");
        CHECK(got.max_mtime == 7 && got.max_ctime == 9, "materialized times");
    }
    efs_kv_mem_free(kv);
}

/* The undecided set is what lets a validated collect claim its values
 * belonged to one instant. Intents exist from PREPARE, so no key version
 * changes when a transaction is decided — only the decision moves, and a
 * reader that took one lane before it and another after it has mixed two
 * states of one atomic transaction. */
static void test_pending_recheck(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct kvs ctx;
    struct efs_txid t;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0, coord;
    struct efs_txn_reduce red = { .max_end = 700 }, got;
    struct efs_txn_pending pend;
    int moved = 1;

    tid(&t, 11);
    ctx.kv = kv;
    ctx.fail = 0;
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_lane(1, 2, 1, 0, key, &klen) == EFS_OK, "lane");
    CHECK(efs_txn_prepare_reduce(kv, &t, &p, key, klen, &red) == EFS_OK, "prep");
    memset(&pend, 0, sizeof(pend));
    CHECK(efs_txn_reduce_read_ex(kv, key, klen, coord_ok, &ctx, &got, &pend) ==
              EFS_OK,
          "read");
    CHECK(got.max_end == 0, "undecided contributes nothing");
    CHECK(pend.n == 1 && pend.overflow == 0, "recorded as undecided");
    CHECK(efs_txn_pending_recheck(&pend, coord_ok, &ctx, &moved) == EFS_OK &&
              moved == 0,
          "still undecided → the collect stands");

    coord = efs_txn_coordinator(&t, &p);
    CHECK(efs_txn_decide(kv, coord, &t, EFS_TXN_COMMIT) == EFS_OK, "decide");
    CHECK(efs_txn_pending_recheck(&pend, coord_ok, &ctx, &moved) == EFS_OK &&
              moved == 1,
          "decision landed → the collect must be retried");

    /* An unreachable coordinator during the recheck is a resource failure,
     * not "nothing moved" — claiming stability we cannot observe would ship
     * exactly the torn vector this check exists to prevent. */
    ctx.fail = 1;
    CHECK(efs_txn_pending_recheck(&pend, coord_ok, &ctx, &moved) == EFS_ERR_IO,
          "I9 on recheck");
    ctx.fail = 0;

    /* Overflow cannot report stability it did not track. */
    memset(&pend, 0, sizeof(pend));
    pend.overflow = 1;
    CHECK(efs_txn_pending_recheck(&pend, coord_ok, &ctx, &moved) == EFS_OK &&
              moved == 1,
          "overflow reports moved");
    efs_kv_mem_free(kv);
}

static void test_guard_vs_excl(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid g, w;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX], val[1] = { 1 };
    uint32_t klen = 0;

    tid(&g, 7);
    tid(&w, 8);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_dseq(1, EFS_ROOT_INO, 0, key, &klen) == EFS_OK, "dseq");
    CHECK(efs_txn_prepare_guard(kv, &g, &p, key, klen, 0) == EFS_OK, "guard");
    CHECK(efs_txn_prepare_excl(kv, &w, &p, key, klen, 0, EFS_TXN_PUT, val, 1) ==
              EFS_ERR_BUSY,
          "phantom: insert vs RMDIR guard");
    CHECK(efs_txn_drop(kv, &g, 1) == EFS_OK, "drop guard");
    CHECK(efs_txn_prepare_excl(kv, &w, &p, key, klen, 0, EFS_TXN_PUT, val, 1) ==
              EFS_OK,
          "after abort");
    CHECK(efs_txn_prepare_guard(kv, &g, &p, key, klen, 0) == EFS_ERR_BUSY,
          "guard vs exclusive");
    efs_kv_mem_free(kv);
}

static void test_cas_unversioned(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid t;
    struct efs_txn_parts p = two(1, 1);
    uint8_t key[EFS_KV_KEY_MAX], live[4] = { 1, 2, 3, 4 }, neu[4] = { 5, 6, 7, 8 };
    uint32_t klen = 0;

    tid(&t, 10);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_inode(1, EFS_ROOT_INO, key, &klen) == EFS_OK, "key");
    CHECK(efs_kv_put(kv, key, klen, live, 4) == EFS_OK, "seed");
    CHECK(efs_txn_prepare_excl(kv, &t, &p, key, klen, 0, EFS_TXN_PUT, neu, 4) ==
              EFS_OK,
          "CAS ver 0 live row");
    efs_kv_mem_free(kv);
}

static void test_abort_old_value(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct kvs ctx;
    struct efs_txid t;
    struct efs_txn_parts p = two(1, 2);
    uint8_t key[EFS_KV_KEY_MAX], val[4] = { 7, 7, 7, 7 }, got[8];
    uint32_t klen = 0, n, coord;

    tid(&t, 9);
    ctx.kv = kv;
    ctx.fail = 0;
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_dentry(1, EFS_ROOT_INO, "a", key, &klen) == EFS_OK, "key");
    CHECK(efs_txn_prepare_excl(kv, &t, &p, key, klen, 0, EFS_TXN_PUT, val, 4) ==
              EFS_OK,
          "prep");
    coord = efs_txn_coordinator(&t, &p);
    CHECK(efs_txn_decide(kv, coord, &t, EFS_TXN_ABORT) == EFS_OK, "abort");
    n = sizeof(got);
    CHECK(efs_txn_read(kv, key, klen, coord_ok, &ctx, got, &n) ==
              EFS_ERR_NOT_FOUND,
          "ABORT → old");
    CHECK(efs_txn_resolve(kv, &t, 1, EFS_TXN_ABORT) == EFS_OK, "resolve");
    n = sizeof(got);
    CHECK(efs_kv_get(kv, key, klen, got, &n) == EFS_ERR_NOT_FOUND, "not written");
    efs_kv_mem_free(kv);
}

/* §7.2 parent-row reductions. The bug this replaces (Sep 20, IO-500
 * mdtest): a txn EXCL-PUT a full parent image read before a log-path
 * apply changed the row, won the version CAS (the log path is
 * unversioned) and overwrote the apply — parent nlink=2 with three live
 * subdirectories. As reductions the deltas fold into the row as it is at
 * RESOLVE, so they commute with the log path and with each other. */
static void put_dir_row(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                        efs_ino_t ino, uint32_t nlink, uint32_t nents,
                        uint64_t mtime)
{
    struct efs_meta_row r;
    uint8_t v[EFS_META_INO_BYTES];

    memset(&r, 0, sizeof(r));
    r.ino = ino;
    r.generation = 1;
    r.mode = 040755;
    r.nlink = nlink;
    r.nents = nents;
    r.layout = EFS_META_LAYOUT_LOCAL;
    r.base_mtime = mtime;
    r.base_ctime = mtime;
    r.parent = 1;
    CHECK(efs_meta_pack_inode(&r, v, sizeof(v)) == EFS_OK, "pack row");
    CHECK(efs_kv_put(kv, key, klen, v, sizeof(v)) == EFS_OK, "put row");
}

static int get_dir_row(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                       struct efs_meta_row *r)
{
    uint8_t v[EFS_META_INO_BYTES];
    uint32_t n = sizeof(v);

    if (efs_kv_get(kv, key, klen, v, &n) != EFS_OK)
        return -1;
    return efs_meta_unpack_inode(v, n, r) == EFS_OK ? 0 : -1;
}

static void test_ino_delta_commutes(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a, b;
    struct efs_txn_parts p = two(1, 2);
    struct efs_txn_ino_delta da, db;
    struct efs_meta_row r;
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen = 0;
    uint64_t ver0 = 0, ver1 = 0;

    tid(&a, 21);
    tid(&b, 22);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_inode(1, 824, key, &klen) == EFS_OK, "inode key");
    put_dir_row(kv, key, klen, 824, 4, 2, 1000);
    CHECK(efs_txn_ver_get(kv, key, klen, &ver0) == EFS_OK, "ver0");

    /* two mkdirs in one parent: neither blocks the other */
    memset(&da, 0, sizeof(da));
    da.d_nlink = 1;
    da.d_nents = 1;
    da.max_mtime = 2000;
    da.max_ctime = 2000;
    db = da;
    db.max_mtime = 1500; /* older clock on the other dual-host */
    db.max_ctime = 1500;
    CHECK(efs_txn_prepare_ino_delta(kv, &a, &p, key, klen, &da) == EFS_OK, "pa");
    CHECK(efs_txn_prepare_ino_delta(kv, &b, &p, key, klen, &db) == EFS_OK,
          "pb does not block on pa");

    /* a same-group log-path rmdir applies in between, unversioned:
     * nlink 4 -> 3, nents 2 -> 1, mtime 3000 */
    put_dir_row(kv, key, klen, 824, 3, 1, 3000);

    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &p), &a, EFS_TXN_COMMIT) ==
              EFS_OK,
          "da");
    CHECK(efs_txn_resolve(kv, &a, 1, EFS_TXN_COMMIT) == EFS_OK, "ra");
    CHECK(get_dir_row(kv, key, klen, &r) == 0, "row after a");
    CHECK(r.nlink == 4 && r.nents == 2, "a folded onto the log-path result");
    CHECK(r.base_mtime == 3000, "MAX keeps the newer log-path time");
    CHECK(efs_txn_ver_get(kv, key, klen, &ver1) == EFS_OK && ver1 == ver0 + 1,
          "fold bumps the row version (a stale EXCL must land STALE)");

    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&b, &p), &b, EFS_TXN_COMMIT) ==
              EFS_OK,
          "db");
    CHECK(efs_txn_resolve(kv, &b, 1, EFS_TXN_COMMIT) == EFS_OK, "rb");
    CHECK(get_dir_row(kv, key, klen, &r) == 0, "row after b");
    CHECK(r.nlink == 5 && r.nents == 3, "b folded too: 3 + 1 + 1");
    CHECK(r.base_mtime == 3000, "older clock does not move mtime back");

    /* an EXCL that read the row before the folds must not be able to
     * overwrite them */
    {
        struct efs_txid c;
        uint8_t v[EFS_META_INO_BYTES];
        memset(v, 0, sizeof(v));
        tid(&c, 23);
        CHECK(efs_txn_prepare_excl(kv, &c, &p, key, klen, ver0, EFS_TXN_PUT, v,
                                   sizeof(v)) == EFS_ERR_STALE,
              "EXCL at the pre-fold version is STALE");
    }
    efs_kv_mem_free(kv);
}

/* Reduce vs. exclusive on one row: an EXCL (rmdir's DEL of the child row)
 * must not be prepared over a pending reduce (a create's nlink++ into that
 * child), and a reduce must not be prepared over a pending EXCL. GUARD
 * likewise. Both directions are BUSY, never a silent overwrite. */
static void test_ino_delta_vs_excl_guard(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a, b, c;
    struct efs_txn_parts p = two(1, 2);
    struct efs_txn_ino_delta d;
    uint8_t key[EFS_KV_KEY_MAX], dk[EFS_KV_KEY_MAX];
    uint32_t klen = 0, dl = 0;
    uint64_t seq = 0;

    tid(&a, 31);
    tid(&b, 32);
    tid(&c, 33);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_inode(1, 900, key, &klen) == EFS_OK, "inode key");
    put_dir_row(kv, key, klen, 900, 2, 0, 10);
    memset(&d, 0, sizeof(d));
    d.d_nlink = 1;
    CHECK(efs_txn_key_busy(kv, key, klen) == EFS_OK, "free row is not busy");
    CHECK(efs_txn_prepare_ino_delta(kv, &a, &p, key, klen, &d) == EFS_OK, "pa");
    CHECK(efs_txn_key_busy(kv, key, klen) == EFS_ERR_BUSY,
          "log path sees the pending reduce");
    CHECK(efs_txn_prepare_excl(kv, &b, &p, key, klen, 0, EFS_TXN_DEL, NULL, 0) ==
              EFS_ERR_BUSY,
          "EXCL over a pending reduce is BUSY");
    CHECK(efs_txn_prepare_guard(kv, &b, &p, key, klen, 0) == EFS_ERR_BUSY,
          "GUARD over a pending reduce is BUSY");
    CHECK(efs_txn_drop(kv, &a, 1) == EFS_OK, "drop a");
    CHECK(efs_txn_key_busy(kv, key, klen) == EFS_OK, "dropped reduce frees it");
    CHECK(efs_txn_prepare_excl(kv, &b, &p, key, klen, 0, EFS_TXN_DEL, NULL, 0) ==
              EFS_OK,
          "EXCL after drop");
    CHECK(efs_txn_prepare_ino_delta(kv, &c, &p, key, klen, &d) == EFS_ERR_BUSY,
          "reduce over a pending EXCL is BUSY");
    CHECK(efs_txn_key_busy(kv, key, klen) == EFS_ERR_BUSY,
          "log path sees the pending intent");

    /* dseq witness: GUARD by value, +1 as a reduce, GUARD/ADD exclusive */
    CHECK(efs_kv_key_dseq(1, 900, 0, dk, &dl) == EFS_OK, "dseq key");
    CHECK(efs_txn_dseq_observe(kv, dk, dl, &seq) == EFS_OK && seq == 0,
          "absent witness observes 0");
    CHECK(efs_txn_prepare_add(kv, &c, &p, dk, dl, 1) == EFS_OK, "add prep");
    CHECK(efs_txn_prepare_guard(kv, &a, &p, dk, dl, 0) == EFS_ERR_BUSY,
          "GUARD over a pending add is BUSY");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&c, &p), &c, EFS_TXN_COMMIT) ==
              EFS_OK,
          "dc");
    CHECK(efs_txn_resolve(kv, &c, 1, EFS_TXN_COMMIT) == EFS_OK, "rc");
    CHECK(efs_txn_dseq_observe(kv, dk, dl, &seq) == EFS_OK && seq == 1,
          "add folded");
    CHECK(efs_txn_prepare_guard(kv, &a, &p, dk, dl, 0) == EFS_ERR_STALE,
          "GUARD at the old value is STALE (dir is no longer empty)");
    CHECK(efs_txn_prepare_guard(kv, &a, &p, dk, dl, 1) == EFS_OK,
          "GUARD at the current value holds");
    {
        /* an unversioned log-path bump is seen by value too */
        uint8_t v[8] = { 0, 0, 0, 0, 0, 0, 0, 2 };
        CHECK(efs_txn_drop(kv, &a, 1) == EFS_OK, "drop guard");
        CHECK(efs_kv_put(kv, dk, dl, v, 8) == EFS_OK, "log-path bump");
        CHECK(efs_txn_prepare_guard(kv, &a, &p, dk, dl, 1) == EFS_ERR_STALE,
              "log-path bump invalidates the guard");
    }
    efs_kv_mem_free(kv);
}

/* Lane fold keeps the owner's tail (fenced_epoch, mtime_gen, append_bar)
 * and MAXes mtime_gen; a 56-byte published lane must not come back 32. */
static void test_lane_fold_preserves_tail(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a;
    struct efs_txn_parts p = two(1, 2);
    struct efs_txn_reduce red;
    uint8_t key[EFS_KV_KEY_MAX], lv[56], got[64];
    uint32_t klen = 0, n = sizeof(got);

    tid(&a, 41);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_lane(1, 7, 1, 0, key, &klen) == EFS_OK, "lane");
    memset(lv, 0, sizeof(lv));
    lv[7] = 100;  /* max_end */
    lv[15] = 50;  /* max_mtime */
    lv[23] = 50;  /* max_ctime */
    lv[31] = 3;   /* seq */
    lv[39] = 9;   /* fenced_epoch */
    lv[47] = 2;   /* mtime_gen */
    lv[55] = 77;  /* append_bar */
    CHECK(efs_kv_put(kv, key, klen, lv, sizeof(lv)) == EFS_OK, "put lane");
    memset(&red, 0, sizeof(red));
    red.max_mtime = 60;
    red.max_ctime = 60;
    red.mtime_gen = 4;
    CHECK(efs_txn_prepare_reduce(kv, &a, &p, key, klen, &red) == EFS_OK, "prep");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &p), &a, EFS_TXN_COMMIT) ==
              EFS_OK,
          "decide");
    CHECK(efs_txn_resolve(kv, &a, 1, EFS_TXN_COMMIT) == EFS_OK, "resolve");
    CHECK(efs_kv_get(kv, key, klen, got, &n) == EFS_OK && n == 56,
          "lane keeps its 56 bytes");
    CHECK(got[7] == 100 && got[15] == 60 && got[23] == 60, "MAX triple");
    CHECK(got[31] == 4, "seq++");
    CHECK(got[39] == 9 && got[55] == 77, "fenced_epoch / append_bar kept");
    CHECK(got[47] == 4, "mtime_gen MAX");
    efs_kv_mem_free(kv);
}

/* The shared PREPARE decoder accepts every kind the host packs. */
static void test_apply_prepare_wire(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a;
    struct efs_txn_parts p = two(1, 2);
    struct efs_txn_ino_delta d;
    struct efs_meta_row r;
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_TXN_REDUCE_INO_WIRE];
    uint32_t klen = 0;

    tid(&a, 51);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_inode(1, 77, key, &klen) == EFS_OK, "key");
    put_dir_row(kv, key, klen, 77, 2, 0, 5);
    memset(&d, 0, sizeof(d));
    d.d_nlink = -1;
    d.d_nents = 0;
    d.or_used_shards = 0x10;
    d.set_parent = 4242;
    d.d_pver = 1;
    efs_txn_encode_ino_delta(pay, &d);
    CHECK(efs_txn_apply_prepare(kv, EFS_TXN_REDUCE_INO, &a, &p, key, klen, pay,
                                sizeof(pay)) == EFS_OK,
          "decode + prepare");
    CHECK(efs_txn_apply_prepare(kv, EFS_TXN_REDUCE_INO, &a, &p, key, klen, pay,
                                sizeof(pay) - 1) == EFS_ERR_PROTO,
          "short payload is PROTO");
    CHECK(efs_txn_apply_prepare(kv, 99, &a, &p, key, klen, pay, sizeof(pay)) ==
              EFS_ERR_PROTO,
          "unknown kind is PROTO");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &p), &a, EFS_TXN_COMMIT) ==
              EFS_OK,
          "decide");
    CHECK(efs_txn_resolve(kv, &a, 1, EFS_TXN_COMMIT) == EFS_OK, "resolve");
    CHECK(get_dir_row(kv, key, klen, &r) == 0, "row");
    CHECK(r.nlink == 1 && r.used_shards == 0x10 && r.parent == 4242 &&
              r.parent_version == 1,
          "every delta field folded");
    efs_kv_mem_free(kv);
}

/* Recovery input (L5): the distinct pending txns of one shard, with the
 * part list a host needs to find the coordinator; records of other shards
 * and of resolved txns are not reported; an abort-resolve clears the shard. */
static void test_scan_pending(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a, b;
    struct efs_txn_parts pa = two(1, 2), pb = two(1, 1);
    struct efs_txn_pending_rec recs[4];
    struct efs_txn_ino_delta d;
    uint8_t k1[EFS_KV_KEY_MAX], k2[EFS_KV_KEY_MAX], k3[EFS_KV_KEY_MAX],
        val[4] = { 1, 2, 3, 4 };
    uint32_t l1 = 0, l2 = 0, l3 = 0, n = 99;

    tid(&a, 41);
    tid(&b, 42);
    CHECK(kv != NULL, "kv");
    CHECK(efs_kv_key_dentry(1, EFS_ROOT_INO, "p", k1, &l1) == EFS_OK, "k1");
    CHECK(efs_kv_key_dentry(2, EFS_ROOT_INO, "q", k2, &l2) == EFS_OK, "k2");
    CHECK(efs_kv_key_inode(1, 900, k3, &l3) == EFS_OK, "k3");
    put_dir_row(kv, k3, l3, 900, 2, 0, 10);
    memset(&d, 0, sizeof(d));
    d.d_nlink = 1;
    /* txn a: EXCL on shard 1 and 2, plus a reduce on shard 1 (three records,
     * one txn); txn b: a guard on shard 1. */
    CHECK(efs_txn_prepare_excl(kv, &a, &pa, k1, l1, 0, EFS_TXN_PUT, val, 4) ==
              EFS_OK,
          "a excl 1");
    CHECK(efs_txn_prepare_excl(kv, &a, &pa, k2, l2, 0, EFS_TXN_PUT, val, 4) ==
              EFS_OK,
          "a excl 2");
    CHECK(efs_txn_prepare_ino_delta(kv, &a, &pa, k3, l3, &d) == EFS_OK,
          "a reduce 1");
    CHECK(efs_txn_prepare_guard(kv, &b, &pb, k1, l1, 0) == EFS_ERR_BUSY,
          "b cannot guard a's key");
    {
        uint8_t k4[EFS_KV_KEY_MAX];
        uint32_t l4 = 0;
        CHECK(efs_kv_key_dentry(1, EFS_ROOT_INO, "r", k4, &l4) == EFS_OK, "k4");
        CHECK(efs_txn_prepare_guard(kv, &b, &pb, k4, l4, 0) == EFS_OK, "b guard");
    }
    CHECK(efs_txn_scan_pending(kv, 1, recs, 4, &n) == EFS_OK, "scan 1");
    CHECK(n == 2, "two distinct txns on shard 1 (a has 2 records there)");
    if (n == 2) {
        int ia = recs[0].t.bytes[0] == 41 ? 0 : 1;
        CHECK(recs[ia].t.bytes[0] == 41 && recs[1 - ia].t.bytes[0] == 42,
              "both txids reported");
        CHECK(recs[ia].parts.n == 2 && recs[ia].parts.shard[1] == 2,
              "a's part list carries shard 2");
        CHECK(efs_txn_coordinator(&recs[ia].t, &recs[ia].parts) ==
                  efs_txn_coordinator(&a, &pa),
              "coordinator recomputable from the record");
    }
    CHECK(efs_txn_scan_pending(kv, 2, recs, 4, &n) == EFS_OK && n == 1 &&
              recs[0].t.bytes[0] == 41,
          "shard 2 sees only a");
    CHECK(efs_txn_scan_pending(kv, 3, recs, 4, &n) == EFS_OK && n == 0,
          "shard 3 is clean");
    /* one-slot page: full page is not an error, caller sweeps again */
    CHECK(efs_txn_scan_pending(kv, 1, recs, 1, &n) == EFS_OK && n == 1,
          "page of one");
    /* what recovery does: decide ABORT at the coordinator, resolve parts */
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &pa), &a, EFS_TXN_ABORT) ==
              EFS_OK,
          "abort a");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &pa), &a, EFS_TXN_COMMIT) ==
              EFS_ERR_PROTO,
          "a later COMMIT of an aborted txn is PROTO (recovery relies on it)");
    CHECK(efs_txn_resolve(kv, &a, 1, EFS_TXN_ABORT) == EFS_OK, "res a 1");
    CHECK(efs_txn_resolve(kv, &a, 2, EFS_TXN_ABORT) == EFS_OK, "res a 2");
    CHECK(efs_txn_resolve(kv, &a, 2, EFS_TXN_ABORT) == EFS_OK,
          "resolve of a resolved shard is idempotent");
    CHECK(efs_txn_scan_pending(kv, 1, recs, 4, &n) == EFS_OK && n == 1 &&
              recs[0].t.bytes[0] == 42,
          "only b remains on shard 1");
    CHECK(efs_txn_scan_pending(kv, 2, recs, 4, &n) == EFS_OK && n == 0,
          "shard 2 clean after abort");
    CHECK(efs_txn_prepare_guard(kv, &b, &pb, k1, l1, 0) == EFS_OK,
          "a's key is free again");
    efs_kv_mem_free(kv);
}

/* I16 op-id window as a txn REDUCE: COMMIT folds the verdict into whatever
 * the window holds (including a log-path record that landed in between),
 * ABORT records nothing, and the wire form round-trips through
 * efs_txn_apply_prepare. */
static void test_opid_reduce(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid a, b;
    struct efs_txn_parts p = two(1, 2);
    struct efs_opid_req q1, q2, q3;
    struct efs_opid_reply rep, got;
    uint8_t uuid[EFS_OPID_UUID_LEN], key[EFS_KV_KEY_MAX];
    uint8_t cur[EFS_OPID_VAL_MAX], out[EFS_OPID_VAL_MAX];
    uint8_t pay[EFS_TXN_REDUCE_OPID_WIRE];
    uint32_t klen = 0, cn, on;
    int rc;

    tid(&a, 51);
    tid(&b, 52);
    CHECK(kv != NULL, "kv");
    memset(uuid, 0, sizeof(uuid));
    uuid[3] = 7;
    CHECK(efs_kv_key_opid(1, uuid, 1, key, &klen) == EFS_OK, "opid key");
    memset(&q1, 0, sizeof(q1));
    memcpy(q1.id.client_uuid, uuid, EFS_OPID_UUID_LEN);
    q1.id.session_epoch = 1;
    q1.id.seq = 1;
    q2 = q1;
    q2.id.seq = 2;
    q3 = q1;
    q3.id.seq = 3;
    q3.ack = 1;
    memset(&rep, 0, sizeof(rep));
    rep.rc = EFS_OK;
    rep.ino = 4242;

    /* no window yet: probe is a miss, not an error */
    CHECK(efs_meta_apply_opid_probe(kv, 1, &q1.id, &got) == 0, "empty probe");
    /* txn a records seq 1 on COMMIT */
    CHECK(efs_txn_prepare_opid(kv, &a, &p, key, klen, &q1, &rep) == EFS_OK,
          "prep a");
    CHECK(efs_meta_apply_opid_probe(kv, 1, &q1.id, &got) == 0,
          "pending reduce is not visible");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &p), &a, EFS_TXN_COMMIT) ==
              EFS_OK,
          "decide a");
    CHECK(efs_txn_resolve(kv, &a, 1, EFS_TXN_COMMIT) == EFS_OK, "resolve a");
    rc = efs_meta_apply_opid_probe(kv, 1, &q1.id, &got);
    CHECK(rc == 1 && got.rc == EFS_OK && got.ino == 4242, "seq 1 recorded");
    CHECK(efs_meta_apply_opid_probe(kv, 1, &q2.id, &got) == 0, "seq 2 new");

    /* txn b prepares seq 2; a log-path fold of seq 3 lands meanwhile */
    rep.ino = 4343;
    CHECK(efs_txn_prepare_opid(kv, &b, &p, key, klen, &q2, &rep) == EFS_OK,
          "prep b");
    cn = sizeof(cur);
    CHECK(efs_kv_get(kv, key, klen, cur, &cn) == EFS_OK, "window present");
    rep.ino = 4444;
    on = sizeof(out);
    CHECK(efs_opid_fold(cur, cn, uuid, 1, &q3, &rep, out, &on) == EFS_OK, "fold 3");
    CHECK(efs_kv_put(kv, key, klen, out, on) == EFS_OK, "log-path put");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&b, &p), &b, EFS_TXN_COMMIT) ==
              EFS_OK,
          "decide b");
    CHECK(efs_txn_resolve(kv, &b, 1, EFS_TXN_COMMIT) == EFS_OK, "resolve b");
    rc = efs_meta_apply_opid_probe(kv, 1, &q2.id, &got);
    CHECK(rc == 1 && got.ino == 4343, "seq 2 folded over the log-path window");
    rc = efs_meta_apply_opid_probe(kv, 1, &q3.id, &got);
    CHECK(rc == 1 && got.ino == 4444, "seq 3 (log path) survived the fold");
    rc = efs_meta_apply_opid_probe(kv, 1, &q1.id, &got);
    CHECK(rc == 1 && got.rc == EFS_OK, "seq 1 still answers (acked stub or cache)");

    /* ABORT leaves the window untouched */
    tid(&a, 53);
    q1.id.seq = 9;
    CHECK(efs_txn_prepare_opid(kv, &a, &p, key, klen, &q1, &rep) == EFS_OK,
          "prep abort");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&a, &p), &a, EFS_TXN_ABORT) ==
              EFS_OK,
          "decide abort");
    CHECK(efs_txn_resolve(kv, &a, 1, EFS_TXN_ABORT) == EFS_OK, "resolve abort");
    CHECK(efs_meta_apply_opid_probe(kv, 1, &q1.id, &got) == 0,
          "aborted op is not recorded");

    /* wire form: encode + apply_prepare = prepare_opid */
    tid(&b, 54);
    q1.id.seq = 10;
    efs_txn_encode_opid(pay, &q1, &rep);
    CHECK(efs_txn_apply_prepare(kv, EFS_TXN_REDUCE_OPID, &b, &p, key, klen, pay,
                                sizeof(pay)) == EFS_OK,
          "apply_prepare opid");
    CHECK(efs_txn_decide(kv, efs_txn_coordinator(&b, &p), &b, EFS_TXN_COMMIT) ==
              EFS_OK,
          "decide wire");
    CHECK(efs_txn_resolve(kv, &b, 1, EFS_TXN_COMMIT) == EFS_OK, "resolve wire");
    rc = efs_meta_apply_opid_probe(kv, 1, &q1.id, &got);
    CHECK(rc == 1 && got.ino == rep.ino, "wire-form record");
    /* wrong kind for the key is INVAL, not a silent no-op */
    CHECK(efs_txn_prepare_opid(kv, &b, &p, key, klen - 1, &q1, &rep) ==
              EFS_ERR_INVAL,
          "non-opid key rejected");
    efs_kv_mem_free(kv);
}

int main(void)
{
    test_opid_reduce();
    test_scan_pending();
    test_excl_conflict_i16();
    test_i17_visible_at_decision();
    test_i9();
    test_reduce_no_block();
    test_pending_recheck();
    test_guard_vs_excl();
    test_cas_unversioned();
    test_abort_old_value();
    test_ino_delta_commutes();
    test_ino_delta_vs_excl_guard();
    test_lane_fold_preserves_tail();
    test_apply_prepare_wire();
    if (failures) {
        fprintf(stderr, "test_txn: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_txn: OK\n");
    return 0;
}
