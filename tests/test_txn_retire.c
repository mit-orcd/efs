/* W83 / K7: transaction decision retirement.
 *
 * 1. Many transactions keep the decision records on a coordinator shard
 *    bounded (live records only for unretired transactions; retired
 *    tombstones only for the retention window).
 * 2. A participant that asks after reclamation gets the defined refusal:
 *    lookups are STALE ("unknown decision"), an intent read is BUSY, a late
 *    DECIDE cannot re-mint the opposite verdict, and nothing is reported as
 *    the old value.
 * 3. Crash/restart: an LSM store that applies the first half of a command
 *    log, closes, reopens and applies the rest holds exactly the same
 *    decision keys and values as a store that applied the log in one run.
 *
 * The "log" is the sequence of replicated commands the host applies
 * (DECIDE with participants, RESOLVE per participant, RETIRE with the
 * proposer's clock), applied through the same library entry points. */
#include "efs/txn.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define SEC 1000000000ull
#define T0 (1000ull * SEC)

/* Host-minted shape: creation time in bytes 0..7 (host order), then a
 * per-transaction discriminator. */
static void mint(struct efs_txid *t, uint64_t born_ns, uint64_t seq)
{
    memcpy(t->bytes, &born_ns, 8);
    memcpy(t->bytes + 8, &seq, 8);
}

static struct efs_txn_parts parts2(uint32_t a, uint32_t b)
{
    struct efs_txn_parts p;

    memset(&p, 0, sizeof(p));
    p.n = 2;
    p.shard[0] = a;
    p.shard[1] = b;
    return p;
}

/* One complete transaction: an EXCL intent on each participant's dentry,
 * DECIDE at the coordinator, RESOLVE on each participant. Returns the
 * coordinator shard. */
static uint32_t run_txn(struct efs_kv *kv, const struct efs_txid *t,
                        const struct efs_txn_parts *p, int dec, int i)
{
    uint8_t key[EFS_KV_KEY_MAX], val[4] = { 1, 2, 3, 4 };
    uint32_t kl = 0, coord, j;
    char name[32];

    snprintf(name, sizeof(name), "f%d", i);
    for (j = 0; j < p->n; j++) {
        CHECK(efs_kv_key_dentry(p->shard[j], EFS_ROOT_INO, name, key, &kl) == EFS_OK,
              "dentry key");
        CHECK(efs_txn_prepare_excl(kv, t, p, key, kl, 0, EFS_TXN_PUT, val, 4) == EFS_OK,
              "prepare");
    }
    coord = efs_txn_coordinator(t, p);
    CHECK(efs_txn_decide_parts(kv, coord, t, p, dec) == EFS_OK, "decide");
    for (j = 0; j < p->n; j++)
        CHECK(efs_txn_resolve(kv, t, p->shard[j], dec) == EFS_OK, "resolve");
    return coord;
}

static uint32_t total_records(struct efs_kv *kv, uint32_t shard, uint32_t *live,
                              uint32_t *retired)
{
    CHECK(efs_txn_decision_count(kv, shard, live, retired) == EFS_OK, "count");
    return *live + *retired;
}

static void test_bounded(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txn_parts p = parts2(5, 9);
    uint32_t live = 0, retired = 0, max_seen = 0, unretired_live = 0;
    const int N = 3000;
    int i;

    for (i = 0; i < N; i++) {
        struct efs_txid t;
        uint64_t now = T0 + (uint64_t)i * SEC; /* one transaction per second */
        uint32_t coord, tot;

        mint(&t, now, (uint64_t)i);
        coord = run_txn(kv, &t, &p, (i % 7) ? EFS_TXN_COMMIT : EFS_TXN_ABORT, i);
        if (i % 50 == 0) {
            /* A transaction whose RETIRE is lost stays live: never by age. */
            unretired_live++;
            continue;
        }
        CHECK(efs_txn_retire(kv, coord, &t, now) == EFS_OK, "retire");
        tot = total_records(kv, 5, &live, &retired) +
              total_records(kv, 9, &live, &retired);
        if (tot > max_seen)
            max_seen = tot;
    }
    {
        uint32_t l5, r5, l9, r9;

        total_records(kv, 5, &l5, &r5);
        total_records(kv, 9, &l9, &r9);
        CHECK(l5 + l9 == unretired_live, "unretired decisions stay live");
        /* 60 s retention at one retire per second, plus one cursor window
         * of lag and the unretired ones. 3000 transactions, bounded set. */
        CHECK(r5 + r9 <= 60 + EFS_TXN_RETIRE_SCAN + 2, "retired tombstones bounded");
        CHECK(max_seen <= unretired_live + 60 + EFS_TXN_RETIRE_SCAN + 2,
              "decision records never exceed the bound while running");
        printf("bounded: %d txns, live=%u retired=%u max_total=%u\n", N,
               l5 + l9, r5 + r9, max_seen);
    }
    /* No transaction intent outlives its RESOLVE. */
    {
        struct efs_txn_pending_rec recs[4];
        uint32_t n = 9;

        CHECK(efs_txn_scan_pending(kv, 5, recs, 4, &n) == EFS_OK && n == 0, "no pending 5");
        CHECK(efs_txn_scan_pending(kv, 9, recs, 4, &n) == EFS_OK && n == 0, "no pending 9");
    }
    efs_kv_mem_free(kv);
}

struct coord_kv {
    struct efs_kv *kv;
};

static int coord_fn(void *user, const struct efs_txid *t, uint32_t shard, int *dec)
{
    return efs_txn_decision_get(((struct coord_kv *)user)->kv, shard, t, dec);
}

static void test_late_participant(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct coord_kv ck = { kv };
    struct efs_txn_parts p = parts2(3, 4);
    struct efs_txid late, old, later;
    uint8_t key[EFS_KV_KEY_MAX], val[4] = { 9, 9, 9, 9 }, got[16];
    uint32_t kl = 0, coord, n, live, retired;
    int dec = -1;

    /* A committed transaction, retired: its verdict is still answered. */
    mint(&late, T0, 1);
    coord = run_txn(kv, &late, &p, EFS_TXN_COMMIT, 1);
    CHECK(efs_txn_retire(kv, coord, &late, T0 + SEC) == EFS_OK, "retire");
    CHECK(efs_txn_retire(kv, coord, &late, T0 + SEC) == EFS_OK, "retire is idempotent");
    CHECK(efs_txn_decision_get(kv, coord, &late, &dec) == EFS_OK &&
              dec == EFS_TXN_COMMIT,
          "retired decision still answers its verdict");
    CHECK(efs_txn_decide(kv, coord, &late, EFS_TXN_ABORT) == EFS_ERR_PROTO,
          "retired decision refuses the opposite verdict");
    CHECK(efs_txn_decide(kv, coord, &late, EFS_TXN_COMMIT) == EFS_OK,
          "retired decision accepts the same verdict");
    total_records(kv, coord, &live, &retired);
    CHECK(live == 0 && retired == 1, "a retired record is a tombstone, not live");

    /* A RETIRE of a decision that never existed mints nothing. */
    mint(&old, T0 - SEC, 2);
    CHECK(efs_txn_retire(kv, coord, &old, T0 + 2 * SEC) == EFS_ERR_NOT_FOUND,
          "retire without decision is NOT_FOUND");
    total_records(kv, coord, &live, &retired);
    CHECK(live == 0 && retired == 1, "nothing minted");

    /* Another transaction retires after the retention: its RETIRE reclaims
     * the first tombstone, deterministically from its own clock. */
    mint(&later, T0 + 100 * SEC, 3);
    coord = run_txn(kv, &later, &p, EFS_TXN_ABORT, 3);
    CHECK(efs_txn_retire(kv, coord, &later, T0 + 100 * SEC) == EFS_OK, "retire later");
    total_records(kv, coord, &live, &retired);
    CHECK(live == 0 && retired == 1, "expired tombstone reclaimed, fresh one kept");

    /* The late participant asks: unknown, never undecided. */
    CHECK(efs_txn_decision_get(kv, coord, &late, &dec) == EFS_ERR_STALE,
          "reclaimed decision is an explicit unknown (STALE)");
    CHECK(efs_txn_decide(kv, coord, &late, EFS_TXN_ABORT) == EFS_ERR_STALE,
          "late DECIDE ABORT cannot re-mint a reclaimed verdict");
    CHECK(efs_txn_decide(kv, coord, &late, EFS_TXN_COMMIT) == EFS_ERR_STALE,
          "late DECIDE COMMIT is refused the same way");
    /* A straggler PREPARE of the reclaimed transaction (it landed after its
     * RESOLVE): a reader must refuse, not answer the old value. */
    CHECK(efs_kv_key_dentry(p.shard[0], EFS_ROOT_INO, "straggler", key, &kl) == EFS_OK,
          "key");
    CHECK(efs_txn_prepare_excl(kv, &late, &p, key, kl, 0, EFS_TXN_PUT, val, 4) == EFS_OK,
          "straggler prepare");
    n = sizeof(got);
    CHECK(efs_txn_read(kv, key, kl, coord_fn, &ck, got, &n) == EFS_ERR_BUSY,
          "read over unknown decision is a retryable refusal");
    /* Recovery's answer to STALE is DROP on that shard: the key returns to
     * its pre-straggler state (absent). */
    CHECK(efs_txn_drop(kv, &late, p.shard[0]) == EFS_OK, "drop straggler");
    n = sizeof(got);
    CHECK(efs_txn_read(kv, key, kl, coord_fn, &ck, got, &n) == EFS_ERR_NOT_FOUND,
          "after drop the key is plainly absent");

    /* A transaction newer than the floor is still ordinarily undecided. */
    {
        struct efs_txid fresh;

        mint(&fresh, T0 + 200 * SEC, 4);
        CHECK(efs_txn_decision_get(kv, coord, &fresh, &dec) == EFS_ERR_NOT_FOUND,
              "newer than floor: undecided, not unknown");
        CHECK(efs_txn_decide_parts(kv, coord, &fresh, &p, EFS_TXN_COMMIT) == EFS_OK,
              "newer than floor decides normally");
    }
    efs_kv_mem_free(kv);
}

/* ---- crash/restart replay -------------------------------------------- */

#define CMD_DECIDE 1
#define CMD_RESOLVE 2
#define CMD_RETIRE 3

struct cmd {
    int op;
    struct efs_txid t;
    struct efs_txn_parts p;
    uint32_t shard;
    int dec;
    uint64_t now;
};

static int apply_cmd(struct efs_kv *kv, const struct cmd *c, int i)
{
    uint8_t key[EFS_KV_KEY_MAX], val[4] = { 7, 7, 7, 7 };
    uint32_t kl = 0, j;
    char name[32];

    switch (c->op) {
    case CMD_DECIDE:
        snprintf(name, sizeof(name), "r%d", i);
        for (j = 0; j < c->p.n; j++) {
            if (efs_kv_key_dentry(c->p.shard[j], EFS_ROOT_INO, name, key, &kl) != EFS_OK ||
                efs_txn_prepare_excl(kv, &c->t, &c->p, key, kl, 0, EFS_TXN_PUT, val, 4) !=
                    EFS_OK)
                return EFS_ERR_IO;
        }
        return efs_txn_decide_parts(kv, c->shard, &c->t, &c->p, c->dec);
    case CMD_RESOLVE:
        return efs_txn_resolve(kv, &c->t, c->shard, c->dec);
    case CMD_RETIRE: {
        int rc = efs_txn_retire(kv, c->shard, &c->t, c->now);
        return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
    }
    }
    return EFS_ERR_INVAL;
}

struct dump {
    char *buf;
    size_t n, cap;
};

static int dump_cb(void *user, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct dump *d = user;

    if (klen < 3 || (key[2] != EFS_KV_KIND_DECISION &&
                     key[2] != EFS_KV_KIND_DECISION_RETIRED &&
                     key[2] != EFS_KV_KIND_DECISION_CURSOR &&
                     key[2] != EFS_KV_KIND_INTENT))
        return 0;
    if (d->n + 8 + klen + vlen > d->cap) {
        d->cap = (d->cap + klen + vlen + 8) * 2;
        d->buf = realloc(d->buf, d->cap);
        if (!d->buf)
            return 1;
    }
    memcpy(d->buf + d->n, &klen, 4);
    memcpy(d->buf + d->n + 4, &vlen, 4);
    memcpy(d->buf + d->n + 8, key, klen);
    memcpy(d->buf + d->n + 8 + klen, val, vlen);
    d->n += 8 + klen + vlen;
    return 0;
}

static void rmtree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char path[1024];

    if (!d)
        return;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static struct efs_kv *open_lsm(const char *dir)
{
    struct efs_kv_lsm_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    cfg.memtable_max = 16 << 10; /* force flushes: segments, not just WAL */
    cfg.l0_max = 3;
    return efs_kv_lsm_open(dir, &cfg);
}

static void test_crash_restart_replay(void)
{
    enum { NTX = 400 };
    struct cmd *log = calloc((size_t)NTX * 4, sizeof(*log));
    struct efs_kv *ref = efs_kv_mem_create(), *kv;
    struct efs_txn_parts p = parts2(6, 7);
    struct dump a = { 0 }, b = { 0 };
    char dir[256];
    int n = 0, i, half;

    CHECK(log && ref, "alloc");
    for (i = 0; i < NTX; i++) {
        uint64_t now = T0 + (uint64_t)i * (SEC / 2);
        struct cmd c;
        uint32_t j;

        memset(&c, 0, sizeof(c));
        mint(&c.t, now, (uint64_t)i + 77);
        c.p = p;
        c.dec = (i % 5) ? EFS_TXN_COMMIT : EFS_TXN_ABORT;
        c.op = CMD_DECIDE;
        c.shard = efs_txn_coordinator(&c.t, &p);
        log[n++] = c;
        for (j = 0; j < p.n; j++) {
            c.op = CMD_RESOLVE;
            c.shard = p.shard[j];
            log[n++] = c;
        }
        if (i % 13 == 0)
            continue; /* lost RETIRE: stays live in both runs */
        c.op = CMD_RETIRE;
        c.shard = efs_txn_coordinator(&c.t, &p);
        c.now = now;
        log[n++] = c;
    }
    snprintf(dir, sizeof(dir), "/tmp/efs-txn-retire-%d", (int)getpid());
    rmtree(dir);
    for (i = 0; i < n; i++)
        CHECK(apply_cmd(ref, &log[i], i) == EFS_OK, "reference apply");

    /* Crash in the middle of a retirement sweep, then restart. */
    half = n / 2;
    while (half < n && log[half].op != CMD_RETIRE)
        half++;
    half++;
    kv = open_lsm(dir);
    CHECK(kv != NULL, "lsm open");
    for (i = 0; kv && i < half; i++)
        CHECK(apply_cmd(kv, &log[i], i) == EFS_OK, "first run apply");
    efs_kv_lsm_close(kv);
    kv = open_lsm(dir);
    CHECK(kv != NULL, "lsm reopen");
    for (i = half; kv && i < n; i++)
        CHECK(apply_cmd(kv, &log[i], i) == EFS_OK, "after restart apply");

    CHECK(efs_kv_scan(ref, dump_cb, &a) == EFS_OK, "dump ref");
    if (kv)
        CHECK(efs_kv_scan(kv, dump_cb, &b) == EFS_OK, "dump restarted");
    CHECK(a.n > 0 && a.n == b.n && !memcmp(a.buf, b.buf, a.n),
          "restarted replica holds the same decision keys and values");
    {
        uint32_t l6, r6, l7, r7;

        total_records(ref, 6, &l6, &r6);
        total_records(ref, 7, &l7, &r7);
        CHECK(l6 + l7 == (NTX + 12) / 13, "lost RETIREs stay live after restart");
        printf("replay: %d commands, crash after %d, live=%u retired=%u, %zu bytes identical\n",
               n, half, l6 + l7, r6 + r7, a.n);
    }
    if (kv)
        efs_kv_lsm_close(kv);
    rmtree(dir);
    efs_kv_mem_free(ref);
    free(a.buf);
    free(b.buf);
    free(log);
}

/* The sweeper's input: live decisions with their recorded participants. */
static void test_scan_decisions(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txn_parts p = parts2(11, 12);
    struct efs_txn_decision_rec recs[4];
    struct efs_txn_decision_scan sc;
    struct efs_txn_decision_page pg;
    struct efs_txid t;
    uint32_t coord, n = 0;

    memset(&sc, 0, sizeof(sc));
    mint(&t, T0, 5);
    coord = run_txn(kv, &t, &p, EFS_TXN_COMMIT, 5);
    CHECK(efs_txn_scan_decisions(kv, coord, &sc, 64, recs, 4, &n, &pg) == EFS_OK && n == 1 &&
              recs[0].decision == EFS_TXN_COMMIT && recs[0].parts.n == 2 &&
              recs[0].parts.shard[0] == 11 && recs[0].parts.shard[1] == 12 &&
              !memcmp(recs[0].t.bytes, t.bytes, 16) && pg.complete && pg.live == 1,
          "live decision carries verdict and participants");
    CHECK(efs_txn_retire(kv, coord, &t, T0) == EFS_OK, "retire");
    CHECK(efs_txn_scan_decisions(kv, coord, &sc, 64, recs, 4, &n, &pg) == EFS_OK && n == 0 &&
              pg.complete && pg.live == 0,
          "retired decision is no longer swept");
    CHECK(efs_txn_scan_decisions(kv, coord, &sc, 1, recs, 4, &n, &pg) == EFS_ERR_INVAL,
          "a work bound below two keys is refused");
    efs_kv_mem_free(kv);
}

/* ---- R2: resumable live-decision sweep -------------------------------- */

#define SW_COORD 21u
#define SW_BLOCKED 22u /* unavailable participant */
#define SW_OK 23u
#define SW_NBLOCK 40
#define SW_NOK 20

static int txid_cmp(const void *a, const void *b)
{
    return memcmp(((const struct efs_txid *)a)->bytes, ((const struct efs_txid *)b)->bytes, 16);
}

/* Txids coordinated by SW_COORD for both participant pairs, sorted in key
 * order: the first SW_NBLOCK (the first pages) get the unavailable
 * participant, the later ones a resolvable pair. */
static void sweep_fixture(struct efs_kv *kv, struct efs_txid *ids)
{
    struct efs_txn_parts pb = parts2(SW_COORD, SW_BLOCKED), po = parts2(SW_COORD, SW_OK);
    uint64_t seq = 1000;
    int i = 0;

    while (i < SW_NBLOCK + SW_NOK) {
        mint(&ids[i], T0 + seq * 1000, seq);
        seq++;
        if (ids[i].bytes[0] != 0 && /* key range after the LSM test's history */
            efs_txn_coordinator(&ids[i], &pb) == SW_COORD &&
            efs_txn_coordinator(&ids[i], &po) == SW_COORD)
            i++;
    }
    qsort(ids, SW_NBLOCK + SW_NOK, sizeof(ids[0]), txid_cmp);
    for (i = 0; i < SW_NBLOCK + SW_NOK; i++)
        CHECK(efs_txn_decide_parts(kv, SW_COORD, &ids[i], i < SW_NBLOCK ? &pb : &po,
                                   EFS_TXN_COMMIT) == EFS_OK, "fixture decide");
}

/* One sweeper round over a page: a decision with the unavailable
 * participant fails and stays live; the others retire. */
static int sweep_round(struct efs_kv *kv, struct efs_txn_decision_scan *sc, uint32_t work,
                       uint64_t now, struct efs_txn_decision_page *pg)
{
    struct efs_txn_decision_rec recs[32];
    uint32_t n = 0, i, j;
    int retired = 0;

    CHECK(efs_txn_scan_decisions(kv, SW_COORD, sc, work, recs, 32, &n, pg) == EFS_OK, "page");
    CHECK(pg->visited <= work, "page within the work bound");
    for (i = 0; i < n; i++) {
        for (j = 0; j < recs[i].parts.n; j++)
            if (recs[i].parts.shard[j] == SW_BLOCKED)
                break;
        if (j < recs[i].parts.n)
            continue;
        CHECK(efs_txn_retire(kv, SW_COORD, &recs[i].t, now) == EFS_OK, "sweep retire");
        retired++;
    }
    return retired;
}

static void check_swept(struct efs_kv *kv, const struct efs_txid *ids, uint32_t history,
                        const char *what)
{
    uint32_t live = 0, retired = 0;
    int i, dec, blocked_live = 1, ok_retired = 1;

    for (i = 0; i < SW_NBLOCK + SW_NOK; i++) {
        uint8_t key[EFS_KV_KEY_MAX], v[EFS_TXN_DECISION_LIVE_MAX];
        uint32_t kl = 0, vl = sizeof(v);
        int is_live;

        CHECK(efs_kv_key_decision(SW_COORD, ids[i].bytes, key, &kl) == EFS_OK, "key");
        is_live = efs_kv_get(kv, key, kl, v, &vl) == EFS_OK;
        CHECK(efs_txn_decision_get(kv, SW_COORD, &ids[i], &dec) == EFS_OK &&
                  dec == EFS_TXN_COMMIT, "every verdict is still answered");
        if (i < SW_NBLOCK && !is_live)
            blocked_live = 0;
        if (i >= SW_NBLOCK && is_live)
            ok_retired = 0;
    }
    total_records(kv, SW_COORD, &live, &retired);
    CHECK(blocked_live && live == SW_NBLOCK, what);
    CHECK(ok_retired && retired == SW_NOK + history, what);
}

static void test_sweep_cursor(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    struct efs_txid ids[SW_NBLOCK + SW_NOK];
    struct efs_txn_decision_scan sc, fresh;
    struct efs_txn_decision_page pg;
    int round, retired = 0, wrapped_clean = 0;

    sweep_fixture(kv, ids);
    /* The old behaviour: always the first page. 32 blocked, nothing retires. */
    memset(&fresh, 0, sizeof(fresh));
    for (round = 0; round < 5; round++) {
        memset(&fresh, 0, sizeof(fresh));
        CHECK(sweep_round(kv, &fresh, 1024, T0, &pg) == 0, "first page is all blocked");
    }
    /* With the continuation: later resolvable decisions retire. */
    memset(&sc, 0, sizeof(sc));
    for (round = 0; round < 8; round++)
        retired += sweep_round(kv, &sc, 1024, T0, &pg);
    CHECK(retired == SW_NOK && sc.wraps >= 1, "continuation reaches every later decision");
    check_swept(kv, ids, 0, "blocked decisions stay live, resolvable ones retired");
    /* Blocked ones are retried every wrap and remain; a page never claims
     * the shard is clean while they exist. */
    for (round = 0; round < 6; round++) {
        sweep_round(kv, &sc, 1024, T0, &pg);
        if (pg.complete && !pg.live)
            wrapped_clean = 1;
    }
    CHECK(!wrapped_clean, "a shard with blocked live decisions never looks clean");
    {
        uint64_t before = sc.wraps;
        for (round = 0; round < 4; round++)
            sweep_round(kv, &sc, 1024, T0, &pg);
        CHECK(sc.wraps > before, "blocked decisions are revisited on later rounds");
    }
    printf("sweep cursor: %d blocked first, %d later retired, wraps=%llu\n", SW_NBLOCK,
           retired, (unsigned long long)sc.wraps);
    efs_kv_mem_free(kv);
}

/* LSM store: the work bound counts deletion tombstones, and a cold
 * restart (new cursor, reopened store) still reaches every decision. */
static void test_sweep_cursor_lsm(void)
{
    struct efs_txid ids[SW_NBLOCK + SW_NOK];
    struct efs_txn_decision_scan sc;
    struct efs_txn_decision_page pg;
    struct efs_kv *kv;
    struct efs_kv_lsm_cfg cfg;
    char dir[256];
    int round, retired = 0, i;

    snprintf(dir, sizeof(dir), "/tmp/efs-txn-sweep-%d", (int)getpid());
    rmtree(dir);
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    kv = efs_kv_lsm_open(dir, &cfg); /* default memtable: tombstones stay merged */
    CHECK(kv != NULL, "lsm open");
    if (!kv)
        return;
    /* Retired history first in key order: 300 decisions whose live keys
     * are now tombstones in the live-decision prefix. */
    {
        struct efs_txn_parts po = parts2(SW_COORD, SW_OK);
        struct efs_txid t;
        int made = 0;
        uint64_t seq = 1;

        while (made < 300) {
            memset(&t, 0, sizeof(t));
            t.bytes[0] = 0; /* sorts before the fixture's key range */
            memcpy(t.bytes + 1, &seq, 8);
            seq++;
            if (efs_txn_coordinator(&t, &po) != SW_COORD)
                continue;
            CHECK(efs_txn_decide_parts(kv, SW_COORD, &t, &po, EFS_TXN_COMMIT) == EFS_OK, "h");
            CHECK(efs_txn_retire(kv, SW_COORD, &t, T0) == EFS_OK, "h retire");
            made++;
        }
    }
    sweep_fixture(kv, ids);
    for (i = 0; i < SW_NBLOCK + SW_NOK; i++)
        CHECK(ids[i].bytes[0] != 0, "fixture after history");
    memset(&sc, 0, sizeof(sc));
    CHECK(sweep_round(kv, &sc, 64, T0, &pg) == 0 && !pg.complete && sc.cursor_len,
          "a page over tombstones stops at the work bound and resumes");
    /* Half way round, crash: reopen the store, start with a fresh cursor
     * (as a restarted or newly elected leader does). */
    for (round = 0; round < 4; round++)
        retired += sweep_round(kv, &sc, 64, T0, &pg);
    efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(dir, &cfg);
    CHECK(kv != NULL, "lsm reopen");
    if (!kv)
        return;
    memset(&sc, 0, sizeof(sc));
    for (round = 0; round < 40 && retired < SW_NOK; round++)
        retired += sweep_round(kv, &sc, 64, T0, &pg);
    CHECK(retired == SW_NOK, "after restart every resolvable decision retires");
    check_swept(kv, ids, 300, "after restart: blocked live, resolvable retired");
    printf("sweep cursor lsm: 300 tombstones, work 64, %d retired across a restart\n", retired);
    efs_kv_lsm_close(kv);
    rmtree(dir);
}

int main(void)
{
    test_bounded();
    test_late_participant();
    test_scan_decisions();
    test_sweep_cursor();
    test_sweep_cursor_lsm();
    test_crash_restart_replay();
    if (failures) {
        fprintf(stderr, "test_txn_retire: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_txn_retire: decisions bounded, unknown decision refused, "
           "crash/restart replays identically PASS\n");
    return 0;
}
