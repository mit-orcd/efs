/* W77 / K14: request-wide publication decision and read validation.
 *
 * One file, two chunks on different lane shards (chunk 1's lane is the
 * other metadata group in a cluster; here every shard shares one memory
 * KV). A unit publication PREPAREs both chunks' staged publications and
 * the FileID pin, DECIDEs once at the inode shard and RESOLVEs each shard,
 * through the same library entry points the host applies:
 *
 * 1. Staged requests are invisible, exclusive and idempotent: the stored
 *    mappings stay the previous ones, other writers of a staged chunk and
 *    other transactions are BUSY, the same request re-PREPAREs OK (host
 *    time excluded) and a changed one under the same txid is STALE.
 * 2. The pin keeps truncation and epoch fences BUSY until RETIRE.
 * 3. Readers: an undecided stage reads the previous mapping and is
 *    recorded for the collect's recheck, a committed unresolved one is
 *    BUSY, an aborted one reads the previous mapping, a reclaimed
 *    decision is BUSY, and an unreachable coordinator is an error.
 * 4. COMMIT applies every stage (idempotently), ABORT applies none, a
 *    refused PREPARE leaves nothing, DROP removes stages and pins, and
 *    recovery's pending scan finds a stranded publication. */
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/put_ticket.h"
#include "efs/raft.h"
#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define T0 1000000000000000000ull
#define CS EFS_DEFAULT_CHUNK_SIZE

static const struct efs_meta_attrs g_at = { 1000, 1000, T0 };
static struct efs_opid g_session;

struct fixture {
    struct efs_kv *kv;
    efs_ino_t ino;
    struct efs_meta_row row;
    uint32_t ish, lsh1;
};

static void establish_session(struct efs_kv *kv)
{
    memset(&g_session, 0, sizeof(g_session));
    g_session.client_uuid[15] = 0x77;
    g_session.session_epoch = 1;
    g_session.seq = 1;
    for (uint32_t shard = 0; shard < EFS_SESSION_BITS; shard++)
        (void)efs_session_establish(kv, shard, g_session.client_uuid, 1);
}

/* K13: every staged publication names an admitted PUT ticket; `tag` is the
 * ticket sequence and the object name derives from it (G). */
static uint64_t G(uint32_t ci, uint64_t tag)
{
    return efs_meta_candidate_gen(g_session.client_uuid, 1, tag, ci, 0);
}

static struct efs_meta_pub pub_of(const struct fixture *f, uint32_t ci,
                                  uint64_t expected, uint64_t tag)
{
    struct efs_meta_pub p;
    struct efs_put_ticket t;

    memset(&p, 0, sizeof(p));
    p.ino = f->ino;
    p.chunk_index = ci;
    p.new_size = 2ull * CS;
    p.now = T0 + 10;
    p.expected_gen = expected ? G(ci, expected) : 0;
    p.candidate_gen = G(ci, tag);
    p.content_epoch = f->row.content_epoch;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.inode_gen = f->row.generation;
    p.mtime_gen = f->row.mtime_gen;
    p.lane_local = ci % EFS_META_LANES != 0; /* lane 1 lives off the inode shard */
    p.fresh_object = 1;
    p.ticketed = 1;
    p.put_id = g_session;
    p.put_id.seq = tag;
    p.publication_id = p.put_id;
    p.put_fragment_len = CS / 2;
    p.put_member_mask = 7;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = (efs_node_id_t)(i + 1);
        p.ch.checksums[i][0] = (uint8_t)tag;
    }
    /* Admit once; a published or retired ticket simply stays as it is. */
    if (efs_put_ticket_from_pub(&p, &t) == EFS_OK)
        (void)efs_put_ticket_admit(f->kv, &t);
    return p;
}

static void setup(struct fixture *f)
{
    struct efs_meta_pub p;

    memset(f, 0, sizeof(*f));
    f->kv = efs_kv_mem_create();
    CHECK(f->kv && efs_meta_apply_init(f->kv, T0) == EFS_OK, "init");
    establish_session(f->kv);
    CHECK(efs_meta_apply_create_file(f->kv, &g_at, EFS_ROOT_INO, S_IFREG | 0644,
                                     "unit", &f->ino) == EFS_OK, "create");
    CHECK(efs_meta_apply_get_inode(f->kv, f->ino, &f->row) == EFS_OK, "row");
    f->ish = efs_kv_inode_shard(f->ino);
    f->lsh1 = efs_kv_lane_shard(f->ino, 1);
    CHECK(f->ish == efs_kv_lane_shard(f->ino, 0) && f->lsh1 != f->ish,
          "lane 0 is the inode shard, lane 1 another");
    CHECK(efs_raft_shard_group(f->ish) != efs_raft_shard_group(f->lsh1),
          "adjacent chunks are in different metadata groups");
    CHECK(efs_meta_apply_activate_lanes_at(f->kv, f->ino, 1ull << 1,
                                           f->row.content_epoch) == EFS_OK &&
          efs_meta_apply_bootstrap_lane(f->kv, f->ino, f->row.generation, 1,
                                        f->row.content_epoch) == EFS_OK,
          "lane 1 registered and stamped");
    CHECK(efs_meta_apply_get_inode(f->kv, f->ino, &f->row) == EFS_OK, "row again");
    p = pub_of(f, 0, 0, 0xA0);
    CHECK(efs_meta_apply_publish(f->kv, &p) == EFS_OK, "base chunk 0");
    p = pub_of(f, 1, 0, 0xB0);
    CHECK(efs_meta_apply_publish(f->kv, &p) == EFS_OK, "base chunk 1");
    CHECK(efs_meta_apply_get_inode(f->kv, f->ino, &f->row) == EFS_OK, "row after base");
}

/* The participant order puts the inode shard where the coordinator lands,
 * exactly as the host does. */
static struct efs_txn_parts parts_for(const struct fixture *f, const struct efs_txid *t)
{
    struct efs_txn_parts p;

    memset(&p, 0, sizeof(p));
    p.n = 2;
    uint32_t at = efs_txn_hash(t) % 2;
    p.shard[at] = f->ish;
    p.shard[1 - at] = f->lsh1;
    return p;
}

static void mint(struct efs_txid *t, uint64_t n)
{
    uint64_t born = T0;
    memcpy(t->bytes, &born, 8);
    memcpy(t->bytes + 8, &n, 8);
}

static int prep_chunk(const struct fixture *f, const struct efs_txid *t,
                      const struct efs_txn_parts *parts, const struct efs_meta_pub *p)
{
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_PUB_STAGE_LEN];
    uint32_t kl = 0;
    uint8_t lane = (uint8_t)(p->chunk_index % EFS_META_LANES);
    int rc = efs_meta_pub_stage_encode(p, &g_session, pay);

    if (rc == EFS_OK)
        rc = efs_kv_key_chunk(efs_kv_lane_shard(p->ino, lane), p->ino, p->inode_gen,
                              lane, p->chunk_index, key, &kl);
    return rc == EFS_OK ? efs_txn_apply_prepare(f->kv, EFS_TXN_PUBLISH, t, parts, key,
                                                kl, pay, sizeof(pay))
                        : rc;
}

static int prep_pin(const struct fixture *f, const struct efs_txid *t,
                    const struct efs_txn_parts *parts, uint64_t epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], pin[EFS_PUB_PIN_LEN];
    uint32_t kl = 0;

    efs_meta_pub_pin_encode(f->row.generation, epoch, pin);
    if (efs_kv_key_inode(f->ish, f->ino, key, &kl) != EFS_OK)
        return EFS_ERR_INVAL;
    return efs_txn_apply_prepare(f->kv, EFS_TXN_PUBLISH, t, parts, key, kl, pin,
                                 sizeof(pin));
}

static uint64_t gen_of(const struct fixture *f, uint32_t ci)
{
    struct efs_meta_chunk ch;
    return efs_meta_apply_get_chunk(f->kv, f->ino, ci, &ch) == EFS_OK ? ch.generation : 0;
}

/* Coordinator lookup on the same KV; `fail` simulates an unreachable one. */
struct coord_ctx {
    struct efs_kv *kv;
    int fail;
};

static int coord_fn(void *ctx, const struct efs_txid *t, uint32_t shard, int *dec)
{
    struct coord_ctx *c = ctx;
    if (c->fail)
        return EFS_ERR_IO;
    return efs_txn_decision_get(c->kv, shard, t, dec);
}

static int epoch_fence(const struct fixture *f)
{
    return efs_meta_apply_epoch_fence(f->kv, f->ino);
}

static int shrink(const struct fixture *f)
{
    struct efs_meta_row r;
    struct efs_meta_truncate_lazy t;
    uint64_t e = 0;
    static uint64_t seq;

    if (efs_meta_apply_get_inode(f->kv, f->ino, &r) != EFS_OK)
        return EFS_ERR_IO;
    memset(&t, 0, sizeof(t));
    t.ino = f->ino;
    t.inode_gen = r.generation;
    t.expect_epoch = r.content_epoch;
    t.expect_base_size = r.base_size;
    t.size = CS / 2;
    t.mtime_ns = T0 + 100 + seq;
    t.kind = EFS_META_TRUNCATE_SHRINK;
    t.q.id = g_session;
    t.q.id.seq = ++seq;
    t.q.ack = seq - 1;
    return efs_meta_apply_truncate_lazy(f->kv, &t, &e);
}

static void test_encoding(void)
{
    struct fixture f;
    struct efs_meta_pub p, q;
    struct efs_opid s;
    uint8_t pay[EFS_PUB_STAGE_LEN];

    setup(&f);
    p = pub_of(&f, 1, 0xB0, 0xB1);
    p.delta_off = 7;
    p.delta_len = 9;
    p.delta_base_n = 2;
    p.delta_base_seq = 33;
    p.fresh_object = 1;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_OK, "encode");
    CHECK(efs_meta_pub_stage_decode(pay, sizeof(pay), &q, &s) == EFS_OK, "decode");
    CHECK(q.ino == p.ino && q.chunk_index == 1 && q.expected_gen == G(1, 0xB0) &&
          q.candidate_gen == G(1, 0xB1) && q.inode_gen == p.inode_gen && q.lane_local &&
          q.fresh_object && q.delta_off == 7 && q.delta_len == 9 &&
          q.delta_base_n == 2 && q.delta_base_seq == 33 &&
          !memcmp(q.ch.checksums, p.ch.checksums, sizeof(q.ch.checksums)) &&
          !memcmp(s.client_uuid, g_session.client_uuid, sizeof(s.client_uuid)) &&
          s.session_epoch == 1 && q.ticketed && q.put_id.seq == 0xB1 &&
          q.put_fragment_len == CS / 2 && q.put_member_mask == 7 &&
          !memcmp(&q.put_id, &q.publication_id, sizeof(q.put_id)), "round trip");
    p.missing_mask = 4;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_OK &&
          efs_meta_pub_stage_decode(pay, sizeof(pay), &q, NULL) == EFS_OK &&
          q.missing_mask == 4, "the W154 missing mask rides the stage");
    p.missing_mask = 5;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_ERR_INVAL,
          "two missing fragments are never staged");
    p.missing_mask = 0;
    p.ticketed = 0;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_ERR_INVAL,
          "K13: an unticketed publication is never staged");
    p.ticketed = 1;
    p.put_id.session_epoch = 2;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_ERR_INVAL,
          "the ticket stream is the REPORT session's");
    p.put_id.session_epoch = 1;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_OK, "encode again");
    pay[0] = 9;
    CHECK(efs_meta_pub_stage_decode(pay, sizeof(pay), &q, NULL) == EFS_ERR_PROTO,
          "unknown version refused");
    CHECK(efs_meta_pub_stage_decode(pay, sizeof(pay) - 1, &q, NULL) == EFS_ERR_PROTO,
          "short payload refused");
    p.durable_result = 1;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_ERR_INVAL,
          "a receipt publication is not staged");
    p.durable_result = 0;
    p.inode_gen = 0;
    CHECK(efs_meta_pub_stage_encode(&p, &g_session, pay) == EFS_ERR_INVAL,
          "a stage names its FileID");
    efs_kv_mem_free(f.kv);
}

static void test_commit(void)
{
    struct fixture f;
    struct efs_txid t, t2;
    struct efs_txn_parts parts, parts2;
    struct efs_meta_pub p0, p1, other;
    struct efs_txn_pending pend;
    struct coord_ctx cc;
    struct efs_txn_pending_rec recs[8];
    uint32_t n = 0;
    int moved = -1;

    setup(&f);
    cc.kv = f.kv;
    cc.fail = 0;
    mint(&t, 1);
    parts = parts_for(&f, &t);
    CHECK(efs_txn_coordinator(&t, &parts) == f.ish, "coordinator is the inode shard");
    p0 = pub_of(&f, 0, 0xA0, 0xA1);
    p1 = pub_of(&f, 1, 0xB0, 0xB1);
    CHECK(prep_chunk(&f, &t, &parts, &p0) == EFS_OK, "stage chunk 0");
    CHECK(prep_chunk(&f, &t, &parts, &p1) == EFS_OK, "stage chunk 1");
    CHECK(prep_pin(&f, &t, &parts, f.row.content_epoch) == EFS_OK, "pin");
    {
        /* K13: the owner cannot abandon a ticket a pending stage may publish. */
        struct efs_put_ticket tk;
        uint32_t state = 0;
        CHECK(efs_put_ticket_from_pub(&p1, &tk) == EFS_OK &&
              efs_put_ticket_settle(f.kv, &tk) == EFS_ERR_BUSY &&
              efs_put_ticket_get(f.kv, &tk, &state) == EFS_OK &&
              state == EFS_PUT_TICKET_ADMITTED, "settle waits for a staged ticket");
    }

    /* 1. invisible, exclusive, idempotent */
    CHECK(gen_of(&f, 0) == G(0, 0xA0) && gen_of(&f, 1) == G(1, 0xB0), "previous mappings stay");
    other = pub_of(&f, 1, 0xB0, 0xC1);
    CHECK(efs_meta_apply_publish(f.kv, &other) == EFS_ERR_BUSY,
          "an ordinary writer of a staged chunk is BUSY");
    mint(&t2, 2);
    parts2 = parts_for(&f, &t2);
    CHECK(prep_chunk(&f, &t2, &parts2, &other) == EFS_ERR_BUSY,
          "another publication of a staged chunk is BUSY");
    CHECK(prep_chunk(&f, &t, &parts, &p1) == EFS_OK, "same request re-PREPAREs");
    other = p1;
    other.now += 5;
    CHECK(prep_chunk(&f, &t, &parts, &other) == EFS_OK,
          "host time is not part of the request");
    other = p1;
    other.candidate_gen = 0xB9;
    CHECK(prep_chunk(&f, &t, &parts, &other) == EFS_ERR_STALE,
          "a changed request under one txid is STALE");
    CHECK(prep_pin(&f, &t, &parts, f.row.content_epoch) == EFS_OK, "pin re-PREPAREs");
    CHECK(efs_txn_scan_pending(f.kv, f.lsh1, recs, 8, &n) == EFS_OK && n == 1 &&
          !memcmp(recs[0].t.bytes, t.bytes, EFS_TXN_ID_LEN),
          "recovery finds the staged publication");

    /* 2. the pin keeps the epoch still */
    CHECK(epoch_fence(&f) == EFS_ERR_BUSY, "epoch fence BUSY while pinned");
    CHECK(shrink(&f) == EFS_ERR_BUSY, "shrinking truncate BUSY while pinned");
    CHECK(efs_meta_pub_pinned(f.kv, f.ino, f.row.generation) == EFS_ERR_BUSY, "pinned");

    /* 3. undecided: the previous mapping, recorded for the recheck */
    memset(&pend, 0, sizeof(pend));
    CHECK(efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 0, coord_fn, &cc,
                                     &pend) == EFS_OK &&
          efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 1, coord_fn, &cc,
                                     &pend) == EFS_OK && pend.n == 1,
          "undecided stages read as before, one transaction to recheck");
    cc.fail = 1;
    CHECK(efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 1, coord_fn, &cc,
                                     &pend) == EFS_ERR_IO,
          "an unreachable coordinator is never absence");
    cc.fail = 0;
    CHECK(efs_txn_pending_recheck(&pend, coord_fn, &cc, &moved) == EFS_OK && !moved,
          "still undecided");

    CHECK(efs_txn_decide_parts(f.kv, f.ish, &t, &parts, EFS_TXN_COMMIT) == EFS_OK,
          "decide commit");
    CHECK(efs_txn_pending_recheck(&pend, coord_fn, &cc, &moved) == EFS_OK && moved,
          "the decision moved under the collect");
    CHECK(efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 1, coord_fn, &cc,
                                     NULL) == EFS_ERR_BUSY,
          "committed and unresolved: BUSY, never the old mapping");

    /* 4. resolve, lane first: a reader of the unresolved one stays BUSY */
    CHECK(efs_meta_apply_txn_resolve(f.kv, &t, f.lsh1, EFS_TXN_COMMIT) == EFS_OK,
          "resolve lane 1");
    CHECK(gen_of(&f, 1) == G(1, 0xB1) && gen_of(&f, 0) == G(0, 0xA0), "lane 1 applied");
    CHECK(efs_meta_pub_stage_peek(f.kv, f.ino, f.row.generation, 1, NULL, NULL) ==
              EFS_ERR_NOT_FOUND, "its stage left in the same batch");
    CHECK(efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 0, coord_fn, &cc,
                                     NULL) == EFS_ERR_BUSY,
          "the other chunk is BUSY until resolved");
    CHECK(efs_meta_apply_txn_resolve(f.kv, &t, f.ish, EFS_TXN_COMMIT) == EFS_OK,
          "resolve inode shard");
    CHECK(gen_of(&f, 0) == G(0, 0xA1), "chunk 0 applied");
    CHECK(efs_meta_apply_txn_resolve(f.kv, &t, f.ish, EFS_TXN_COMMIT) == EFS_OK &&
          efs_meta_apply_txn_resolve(f.kv, &t, f.lsh1, EFS_TXN_COMMIT) == EFS_OK &&
          gen_of(&f, 0) == G(0, 0xA1) && gen_of(&f, 1) == G(1, 0xB1), "re-RESOLVE is idempotent");
    {
        struct efs_put_ticket tk;
        uint32_t state = 0;
        CHECK(efs_put_ticket_from_pub(&p1, &tk) == EFS_OK &&
              efs_put_ticket_get(f.kv, &tk, &state) == EFS_OK &&
              state == EFS_PUT_TICKET_PUBLISHED, "K13: RESOLVE COMMIT publishes the ticket");
    }
    CHECK(prep_chunk(&f, &t2, &parts2, &other) == EFS_ERR_STALE,
          "a publication built on the old base is STALE after commit");
    CHECK(shrink(&f) == EFS_ERR_BUSY, "the pin outlives RESOLVE");
    CHECK(efs_meta_apply_txn_retire(f.kv, f.ish, &t, T0 + 1000) == EFS_OK, "retire");
    CHECK(efs_meta_pub_pinned(f.kv, f.ino, f.row.generation) == EFS_OK, "unpinned");
    CHECK(efs_txn_scan_pending(f.kv, f.ish, recs, 8, &n) == EFS_OK && n == 0,
          "nothing left for recovery");
    CHECK(epoch_fence(&f) == EFS_OK, "fences proceed after RETIRE");
    CHECK(efs_meta_apply_check(f.kv) == EFS_OK, "check");
    efs_kv_mem_free(f.kv);
}

static void test_abort_and_refusals(void)
{
    struct fixture f;
    struct efs_txid t;
    struct efs_txn_parts parts, bad;
    struct efs_meta_pub p0, p1;
    struct coord_ctx cc;
    uint32_t n = 0;
    struct efs_txn_pending_rec recs[4];

    setup(&f);
    cc.kv = f.kv;
    cc.fail = 0;
    mint(&t, 3);
    parts = parts_for(&f, &t);
    p0 = pub_of(&f, 0, 0xA0, 0xA2);
    p1 = pub_of(&f, 1, 0xB0, 0xB2);
    CHECK(prep_chunk(&f, &t, &parts, &p0) == EFS_OK &&
          prep_chunk(&f, &t, &parts, &p1) == EFS_OK &&
          prep_pin(&f, &t, &parts, f.row.content_epoch) == EFS_OK, "prepare");
    CHECK(efs_txn_decide_parts(f.kv, f.ish, &t, &parts, EFS_TXN_ABORT) == EFS_OK,
          "decide abort");
    CHECK(efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 1, coord_fn, &cc,
                                     NULL) == EFS_OK, "aborted: the previous mapping");
    CHECK(efs_meta_apply_txn_resolve(f.kv, &t, f.lsh1, EFS_TXN_ABORT) == EFS_OK &&
          efs_meta_apply_txn_resolve(f.kv, &t, f.ish, EFS_TXN_ABORT) == EFS_OK,
          "resolve abort");
    CHECK(gen_of(&f, 0) == G(0, 0xA0) && gen_of(&f, 1) == G(1, 0xB0), "nothing applied");
    CHECK(efs_meta_pub_stage_peek(f.kv, f.ino, f.row.generation, 0, NULL, NULL) ==
              EFS_ERR_NOT_FOUND &&
          efs_meta_pub_stage_peek(f.kv, f.ino, f.row.generation, 1, NULL, NULL) ==
              EFS_ERR_NOT_FOUND, "stages gone");
    CHECK(efs_meta_apply_txn_retire(f.kv, f.ish, &t, T0 + 1000) == EFS_OK &&
          efs_meta_pub_pinned(f.kv, f.ino, f.row.generation) == EFS_OK,
          "retired abort unpins");

    /* A refused PREPARE writes nothing. */
    mint(&t, 4);
    parts = parts_for(&f, &t);
    p1 = pub_of(&f, 1, 0xBF, 0xB3);
    CHECK(prep_chunk(&f, &t, &parts, &p1) == EFS_ERR_STALE, "wrong base is STALE");
    CHECK(efs_meta_pub_stage_peek(f.kv, f.ino, f.row.generation, 1, NULL, NULL) ==
              EFS_ERR_NOT_FOUND, "a refused PREPARE stages nothing");
    CHECK(prep_pin(&f, &t, &parts, f.row.content_epoch + 1) == EFS_ERR_STALE,
          "a pin at another epoch is STALE");
    bad = parts;
    bad.shard[0] = parts.shard[1];
    bad.shard[1] = parts.shard[0];
    if (efs_txn_coordinator(&t, &bad) != f.ish)
        CHECK(prep_pin(&f, &t, &bad, f.row.content_epoch) == EFS_ERR_INVAL,
              "a pin whose shard is not the coordinator is refused");
    {
        uint8_t key[EFS_KV_KEY_MAX], pay[EFS_PUB_STAGE_LEN];
        uint32_t kl = 0;
        p1 = pub_of(&f, 1, 0xB0, 0xB3);
        CHECK(efs_meta_pub_stage_encode(&p1, &g_session, pay) == EFS_OK &&
              efs_kv_key_chunk(f.lsh1, f.ino, f.row.generation, 1, 65, key, &kl) == EFS_OK &&
              efs_txn_apply_prepare(f.kv, EFS_TXN_PUBLISH, &t, &parts, key, kl, pay,
                                    sizeof(pay)) == EFS_ERR_INVAL,
              "a stage under another chunk's key is refused");
    }

    /* DROP removes what an abandoned PREPARE left. */
    p0 = pub_of(&f, 0, 0xA0, 0xA4);
    p1 = pub_of(&f, 1, 0xB0, 0xB4);
    CHECK(prep_chunk(&f, &t, &parts, &p0) == EFS_OK &&
          prep_chunk(&f, &t, &parts, &p1) == EFS_OK &&
          prep_pin(&f, &t, &parts, f.row.content_epoch) == EFS_OK, "prepare again");
    CHECK(efs_txn_scan_pending(f.kv, f.ish, recs, 4, &n) == EFS_OK && n == 1,
          "stranded publication is pending on the inode shard");
    CHECK(efs_txn_drop(f.kv, &t, f.ish) == EFS_OK &&
          efs_txn_drop(f.kv, &t, f.lsh1) == EFS_OK, "drop");
    CHECK(efs_meta_pub_pinned(f.kv, f.ino, f.row.generation) == EFS_OK &&
          efs_meta_pub_stage_peek(f.kv, f.ino, f.row.generation, 0, NULL, NULL) ==
              EFS_ERR_NOT_FOUND &&
          efs_meta_pub_stage_peek(f.kv, f.ino, f.row.generation, 1, NULL, NULL) ==
              EFS_ERR_NOT_FOUND, "drop removed stages and pin");
    CHECK(gen_of(&f, 0) == G(0, 0xA0) && gen_of(&f, 1) == G(1, 0xB0), "drop applied nothing");
    CHECK(efs_meta_apply_check(f.kv) == EFS_OK, "check");
    efs_kv_mem_free(f.kv);
}

/* A reclaimed decision is unknown: a straggler stage is BUSY to readers. */
static void test_unknown_decision(void)
{
    struct fixture f;
    struct efs_txid t;
    struct efs_txn_parts parts;
    struct efs_meta_pub p1;
    struct coord_ctx cc;

    setup(&f);
    cc.kv = f.kv;
    cc.fail = 0;
    mint(&t, 5);
    parts = parts_for(&f, &t);
    p1 = pub_of(&f, 1, 0xB0, 0xB5);
    CHECK(efs_txn_decide_parts(f.kv, f.ish, &t, &parts, EFS_TXN_ABORT) == EFS_OK &&
          efs_txn_retire(f.kv, f.ish, &t, T0) == EFS_OK, "retired decision");
    /* Retire far later reclaims the tombstone (txn.h retention). */
    {
        struct efs_txid other;
        mint(&other, 6);
        CHECK(efs_txn_decide_parts(f.kv, f.ish, &other, &parts, EFS_TXN_ABORT) == EFS_OK &&
              efs_txn_retire(f.kv, f.ish, &other,
                             T0 + 10 * EFS_TXN_DECISION_RETAIN_NS) == EFS_OK,
              "a later retire reclaims the first tombstone");
    }
    int dec = 0;
    CHECK(efs_txn_decision_get(f.kv, f.ish, &t, &dec) == EFS_ERR_STALE,
          "the first decision is reclaimed: unknown");
    CHECK(prep_chunk(&f, &t, &parts, &p1) == EFS_OK, "straggler PREPARE");
    CHECK(efs_meta_pub_stage_visible(f.kv, f.ino, f.row.generation, 1, coord_fn, &cc,
                                     NULL) == EFS_ERR_BUSY,
          "an unknown decision is BUSY, never the old mapping");
    efs_kv_mem_free(f.kv);
}

int main(void)
{
    test_encoding();
    test_commit();
    test_abort_and_refusals();
    test_unknown_decision();
    if (failures) {
        fprintf(stderr, "test_pub_stage: %d failures\n", failures);
        return 1;
    }
    printf("test_pub_stage: staged publication decision, pin, read validation, "
           "commit/abort/drop/recovery PASS\n");
    return 0;
}
