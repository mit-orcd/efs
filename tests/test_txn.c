/* Isolated transaction SM. No sockets, no Raft. */
#include "efs/txn.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
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

int main(void)
{
    test_excl_conflict_i16();
    test_i17_visible_at_decision();
    test_i9();
    test_reduce_no_block();
    test_pending_recheck();
    test_guard_vs_excl();
    test_cas_unversioned();
    test_abort_old_value();
    if (failures) {
        fprintf(stderr, "test_txn: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_txn: OK\n");
    return 0;
}
