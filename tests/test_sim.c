/* Deterministic simulator: RF=3 Raft + KV apply SM + mem store/loop.
 * Same seed must replay the same history. */
#include "efs/sim.h"
#include "efs/opid.h"
#include "efs/common.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include "efs/lock.h"
#include "efs/meta_apply.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static struct efs_sim *mk(uint64_t seed)
{
    struct efs_sim_cfg cfg = { .seed = seed, .nservers = 3, .nclients = 2 };
    return efs_sim_new(&cfg);
}

static void test_replay(void)
{
    struct efs_sim *a = mk(42);
    struct efs_sim *b = mk(42);
    efs_ino_t ia = 0, ib = 0;
    uint64_t ha, hb;

    CHECK(a && b, "create");
    CHECK(efs_sim_create(a, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &ia) == EFS_OK,
          "a create");
    CHECK(efs_sim_create(b, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &ib) == EFS_OK,
          "b create");
    CHECK(ia == ib && ia != 0, "same ino");
    ha = efs_sim_history(a);
    hb = efs_sim_history(b);
    CHECK(ha == hb && ha != 0, "history");
    efs_sim_free(a);
    efs_sim_free(b);
}

static void test_table(void)
{
    struct efs_sim *s = mk(7);
    efs_ino_t a = 0, b = 0, g = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "a", &a) == EFS_OK,
          "create a");
    CHECK(efs_sim_create(s, 1, EFS_ROOT_INO, S_IFREG | 0644, "b", &b) == EFS_OK,
          "create b");
    CHECK(a && b && a != b, "unique inos");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "a", &g) == EFS_ERR_EXIST,
          "dup name");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, "a", &g) == EFS_OK && g == a, "lookup");
    CHECK(efs_sim_unlink(s, 0, EFS_ROOT_INO, "a") == EFS_OK, "unlink");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "a", &g) == EFS_ERR_NOT_FOUND,
          "gone");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_export_mkfs(void)
{
    struct efs_sim *s = mk(91);
    struct efs_meta_stat st;
    efs_ino_t f = 0, g = 0;
    int lid;

    CHECK(s, "mk");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK, "root getattr");
    CHECK(S_ISDIR(st.mode) && st.nlink == 2 && st.ino == EFS_ROOT_INO, "root");
    CHECK(efs_sim_mkfs(s) == EFS_OK, "idempotent mkfs");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK && st.nlink == 2,
          "root after retry");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &f) == EFS_OK,
          "create");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    CHECK(efs_sim_crash(s, lid) == EFS_OK, "crash leader");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK && S_ISDIR(st.mode),
          "root after crash");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "f", &g) == EFS_OK && g == f,
          "file after crash");
    CHECK(efs_sim_restart(s, lid) == EFS_OK, "restart");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK && st.nlink == 2,
          "root after restart");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "f", &g) == EFS_OK && g == f,
          "file after restart");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_restart(void)
{
    struct efs_sim *s = mk(9);
    efs_ino_t a = 0, g = 0;
    int lid;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "keep", &a) == EFS_OK,
          "create");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    CHECK(efs_sim_crash(s, lid) == EFS_OK, "crash leader");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "keep", &g) == EFS_OK && g == a,
          "I2/I10 majority");
    CHECK(efs_sim_crash(s, (lid + 1) % 3) == EFS_OK, "crash second");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "keep", &g) == EFS_ERR_BUSY,
          "no quorum");
    CHECK(efs_sim_restart(s, lid) == EFS_OK, "restart");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "keep", &g) == EFS_OK && g == a,
          "reloaded");
    efs_sim_free(s);
}

static void fill(uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(0xA0 + (i * 17));
}

static void test_publish_roundtrip(void)
{
    struct efs_sim *s = mk(11);
    efs_ino_t ino = 0;
    uint8_t src[64], got[64];

    CHECK(s, "mk");
    fill(src, sizeof(src));
    memset(got, 0, sizeof(got));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "d", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "put");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_OK, "publish");
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK, "read");
    CHECK(memcmp(src, got, sizeof(src)) == 0, "bytes");
    efs_sim_free(s);
}

static void test_orphan(void)
{
    struct efs_sim *s = mk(13);
    efs_ino_t ino = 0;
    uint8_t src[32], got[32];
    struct efs_frag_id id;

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "o", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "put");
    CHECK(efs_sim_frag_id(s, 0, ino, 0, 0, 0, 0, &id) == EFS_OK, "id");
    CHECK(efs_sim_frag_present(s, &id) == 1, "orphan on disk");
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_ERR_NOT_FOUND,
          "unpublished");
    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash");
    CHECK(efs_sim_restart(s, EFS_SIM_META) == EFS_OK, "restart");
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_ERR_NOT_FOUND,
          "still unpublished");
    CHECK(efs_sim_frag_present(s, &id) == 1, "still on disk");
    efs_sim_free(s);
}

static void test_i14(void)
{
    struct efs_sim *s = mk(17);
    efs_ino_t ino = 0;
    uint8_t src[16];

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "p", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), 1) == EFS_OK,
          "put skip 1");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_ERR_IO, "I14");
    efs_sim_free(s);
}

static void test_i25(void)
{
    struct efs_sim *s = mk(19);
    efs_ino_t ino = 0;
    uint8_t src[48], got[48];
    struct efs_frag_id id;

    CHECK(s, "mk");
    fill(src, sizeof(src));
    memset(got, 0, sizeof(got));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "c", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "put");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_OK, "publish");
    CHECK(efs_sim_frag_id(s, 0, ino, 0, 2, 0, 0, &id) == EFS_OK, "id");
    CHECK(efs_sim_corrupt(s, 0, &id) == EFS_OK, "corrupt P");
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK, "read");
    CHECK(memcmp(src, got, sizeof(src)) == 0, "I25 skipped corrupt");
    efs_sim_free(s);
}

static void test_faults(void)
{
    struct efs_sim_cfg cfg = { .seed = 23, .nservers = 3, .nclients = 1,
                               .drop_per_mille = 1000 };
    struct efs_sim *s = efs_sim_new(&cfg);
    efs_ino_t g = 0;
    struct efs_sim *p;

    /* mkfs is a Raft proposal, so 100% drop cannot elect and cannot create
     * the export. Client ops used to return AGAIN on a locally-seeded ROOT. */
    CHECK(s == NULL, "drop cannot mkfs");
    if (s)
        efs_sim_free(s);

    p = mk(29);
    CHECK(p, "part");
    CHECK(efs_sim_partition(p, 0, 1) == EFS_OK, "part 0");
    CHECK(efs_sim_partition(p, 1, 1) == EFS_OK, "part 1");
    CHECK(efs_sim_create(p, 0, EFS_ROOT_INO, S_IFREG | 0644, "y", &g) == EFS_ERR_BUSY,
          "no quorum");
    CHECK(efs_sim_partition(p, 0, 0) == EFS_OK, "part 0 off");
    CHECK(efs_sim_partition(p, 1, 0) == EFS_OK, "part 1 off");
    CHECK(efs_sim_clock_step(p, 100) == EFS_OK, "clock");
    CHECK(efs_sim_now(p) >= 100, "now");
    CHECK(efs_sim_create(p, 0, EFS_ROOT_INO, S_IFREG | 0644, "y", &g) == EFS_OK,
          "after");
    efs_sim_free(p);
}

static void test_opid_window(void)
{
    struct efs_opid_window w;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    struct efs_opid id;
    struct efs_opid_reply r, got;
    int hit;

    memset(uuid, 0, sizeof(uuid));
    uuid[0] = 9;
    efs_opid_window_init(&w, uuid, 1);
    memset(&id, 0, sizeof(id));
    memcpy(id.client_uuid, uuid, EFS_OPID_UUID_LEN);
    id.session_epoch = 1;
    id.seq = 1;
    CHECK(efs_opid_lookup(&w, &id, &got) == 0, "new");
    r.rc = EFS_OK;
    r.ino = 7;
    r.extra = 0;
    CHECK(efs_opid_complete(&w, &id, &r) == EFS_OK, "complete 1");
    hit = efs_opid_lookup(&w, &id, &got);
    CHECK(hit == 1 && got.ino == 7, "replay 1");
    id.seq = 3;
    CHECK(efs_opid_complete(&w, &id, &r) == EFS_OK, "complete 3");
    CHECK(w.highest_contiguous_seq == 1, "gap");
    id.seq = 2;
    r.ino = 8;
    CHECK(efs_opid_complete(&w, &id, &r) == EFS_OK, "complete 2");
    CHECK(w.highest_contiguous_seq == 3, "folded");
    CHECK(efs_opid_ack(&w, 3) == EFS_OK, "ack");
    CHECK(w.ncache == 0, "reclaimed");
    id.seq = 1;
    hit = efs_opid_lookup(&w, &id, &got);
    CHECK(hit == 1 && got.rc == EFS_OK, "acked stub");
    efs_opid_window_init(&w, w.client_uuid, 2);
    CHECK(w.client_uuid[0] == 9 && w.session_epoch == 2, "reinit alias");
}

static void test_i16(void)
{
    struct efs_sim *s = mk(31);
    struct efs_opid op;
    efs_ino_t a = 0, b = 0, c = 0, g = 0;

    CHECK(s, "mk");
    efs_sim_opid_for(s, 0, 1, &op);
    CHECK(efs_sim_create_op(s, 0, &op, EFS_ROOT_INO, S_IFREG | 0644, "once", &a)
              == EFS_OK && a,
          "first");
    CHECK(efs_sim_create_op(s, 0, &op, EFS_ROOT_INO, S_IFREG | 0644, "once", &b)
              == EFS_OK && b == a,
          "replay same name");
    CHECK(efs_sim_create_op(s, 0, &op, EFS_ROOT_INO, S_IFREG | 0644, "other", &c)
              == EFS_OK && c == a,
          "replay other name");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "other", &g) == EFS_ERR_NOT_FOUND,
          "no second inode");
    efs_sim_opid_for(s, 0, 2, &op);
    CHECK(efs_sim_create_op(s, 0, &op, EFS_ROOT_INO, S_IFREG | 0644, "two", &b)
              == EFS_OK && b && b != a,
          "new seq");
    CHECK(efs_sim_opid_forget(s, 0) == EFS_OK, "forget");
    efs_sim_opid_for(s, 0, 1, &op);
    CHECK(efs_sim_create_op(s, 0, &op, EFS_ROOT_INO, S_IFREG | 0644, "once", &c)
              == EFS_OK && c == a,
          "durable after forget");
    efs_sim_free(s);
}

static void test_i1_one_leader(void)
{
    struct efs_sim *s = mk(41);
    efs_ino_t a = 0;
    int i, n = 0, lid;
    uint64_t term;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "i1", &a) == EFS_OK,
          "create");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    term = efs_sim_meta_term(s, lid);
    for (i = 0; i < EFS_SIM_RAFT_N; i++) {
        if (efs_sim_meta_role(s, i) == 2) /* EFS_RAFT_LEADER */
            n++;
    }
    CHECK(n == 1, "I1 one leader");
    CHECK(term >= 1, "term");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_i3_term_fence(void)
{
    struct efs_sim *s = mk(43);
    efs_ino_t a = 0, g = 0;
    int i, lid, nlid;
    uint64_t old_term;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "i3", &a) == EFS_OK,
          "create");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    old_term = efs_sim_meta_term(s, lid);
    CHECK(efs_sim_partition(s, lid, 1) == EFS_OK, "isolate leader");
    nlid = -1;
    for (i = 0; i < 40; i++) {
        efs_sim_meta_tick(s, -1);
        nlid = efs_sim_meta_leader(s);
        if (nlid >= 0 && nlid != lid)
            break;
    }
    CHECK(nlid >= 0 && nlid != lid, "new leader");
    CHECK(efs_sim_meta_term(s, nlid) > old_term, "higher term");
    CHECK(efs_sim_partition(s, lid, 0) == EFS_OK, "heal");
    for (i = 0; i < 20; i++)
        efs_sim_meta_tick(s, -1);
    CHECK(efs_sim_meta_role(s, lid) != 2, "I3 old leader stepped down");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "i3", &g) == EFS_OK && g == a,
          "still there");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_i4_no_quorum_commit(void)
{
    struct efs_sim *s = mk(47);
    efs_ino_t a = 0, g = 0;
    int lid, i;
    uint64_t c0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "i4", &a) == EFS_OK,
          "create");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    c0 = efs_sim_meta_commit(s, lid);
    for (i = 0; i < EFS_SIM_RAFT_N; i++) {
        if (i != lid)
            CHECK(efs_sim_partition(s, i, 1) == EFS_OK, "part follower");
    }
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "stuck", &g) ==
              EFS_ERR_BUSY,
          "I4 unacked");
    CHECK(g == 0, "no ino");
    CHECK(efs_sim_meta_commit(s, lid) == c0, "commit frozen");
    for (i = 0; i < EFS_SIM_RAFT_N; i++) {
        if (i != lid)
            efs_sim_partition(s, i, 0);
    }
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "after", &g) ==
              EFS_OK,
          "after quorum");
    CHECK(efs_sim_meta_commit(s, lid) > c0, "commit moved");
    efs_sim_free(s);
}

static void test_readindex(void)
{
    struct efs_sim *s = mk(53);
    efs_ino_t a = 0, g = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "ri", &a) == EFS_OK,
          "create");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, "ri", &g) == EFS_OK && g == a,
          "ReadIndex other client");
    efs_sim_free(s);
}

static void test_l8_desired_placement(void)
{
    struct efs_sim_cfg cfg = { .seed = 61, .nservers = 5, .nclients = 1 };
    struct efs_sim *s = efs_sim_new(&cfg);
    efs_ino_t a = 0, g = 0;
    int lid;

    CHECK(s, "mk5");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "l8", &a) == EFS_OK,
          "create");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    CHECK(efs_sim_meta_voters(s, lid) == 0x7, "initial RF=3");
    CHECK(efs_sim_ctrl_desired(s) == 0x7, "desired starts 0x7");
    CHECK(efs_sim_ctrl_set_desired(s, 0x1f) == EFS_OK, "L8 set 0x1f");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader after grow");
    CHECK(efs_sim_meta_voters(s, lid) == 0x1f, "actual caught up");
    CHECK(!efs_sim_meta_joint(s, lid), "not joint");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "l8", &g) == EFS_OK && g == a,
          "lookup after grow");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static const char *scatter_nth(efs_ino_t parent, int nth)
{
    static char buf[16];
    int i, found = 0;

    for (i = 0; i < 4096; i++) {
        snprintf(buf, sizeof(buf), "d%d", i);
        if (efs_kv_mkdir_shard(parent, buf, 0) != efs_kv_inode_shard(parent)) {
            if (found == nth)
                return buf;
            found++;
        }
    }
    return "d0";
}

static const char *scatter(efs_ino_t parent)
{
    return scatter_nth(parent, 0);
}

static void test_mkdir_i17(void)
{
    struct efs_sim *s = mk(71);
    efs_ino_t ino = 0, g = 0;
    struct efs_txid t;
    const char *nm;

    CHECK(s, "mk");
    nm = scatter(EFS_ROOT_INO);
    CHECK(efs_kv_mkdir_shard(EFS_ROOT_INO, nm, 0) !=
              efs_kv_inode_shard(EFS_ROOT_INO),
          "two shards");
    CHECK(efs_sim_mkdir_until(s, EFS_ROOT_INO, nm, &t, &ino,
                              EFS_SIM_TXN_PREPARE) == EFS_OK &&
              ino,
          "prepare");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_ERR_NOT_FOUND,
          "I17 no half-apply");
    CHECK(efs_sim_txn_finish(s, &t, 0) == EFS_OK, "L5 abort");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_ERR_NOT_FOUND,
          "aborted");
    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, nm, &ino) == EFS_OK && ino,
          "mkdir");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, nm, &g) == EFS_OK && g == ino,
          "visible");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_mkdir_crash_after_prepare(void)
{
    struct efs_sim *s = mk(73);
    efs_ino_t ino = 0, g = 0;
    struct efs_txid t;
    const char *nm;
    int lid;

    CHECK(s, "mk");
    nm = scatter(EFS_ROOT_INO);
    CHECK(efs_sim_mkdir_until(s, EFS_ROOT_INO, nm, &t, &ino,
                              EFS_SIM_TXN_PREPARE) == EFS_OK,
          "prepare");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    CHECK(efs_sim_crash(s, lid) == EFS_OK, "crash");
    CHECK(efs_sim_restart(s, lid) == EFS_OK, "restart");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_ERR_NOT_FOUND,
          "still old");
    CHECK(efs_sim_txn_finish(s, &t, 0) == EFS_OK, "recover abort");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_ERR_NOT_FOUND,
          "aborted");
    efs_sim_free(s);
}

static void test_mkdir_visible_at_decision(void)
{
    struct efs_sim *s = mk(79);
    efs_ino_t ino = 0, g = 0;
    struct efs_txid t;
    const char *nm;

    CHECK(s, "mk");
    nm = scatter(EFS_ROOT_INO);
    CHECK(efs_sim_mkdir_until(s, EFS_ROOT_INO, nm, &t, &ino,
                              EFS_SIM_TXN_DECISION) == EFS_OK &&
              ino,
          "decide");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_OK && g == ino,
          "visible at decision");
    CHECK(efs_sim_txn_finish(s, &t, 1) == EFS_OK, "resolve");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, nm, &g) == EFS_OK && g == ino,
          "after resolve");
    efs_sim_free(s);
}

static void test_i23_session_fence(void)
{
    struct efs_sim *s = mk(21);
    efs_ino_t ino = 0, b = 0;
    uint8_t src[32], got[32];
    uint32_t i;

    CHECK(s, "mk");
    for (i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(0x40 + i);
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "a", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_session_fence_until(s, 0, EFS_SIM_FENCE_LOCAL) == EFS_OK,
          "local");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "b", &b) ==
              EFS_ERR_STALE,
          "old epoch create");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "PUT after fence");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_ERR_STALE,
          "publish fenced");
    CHECK(efs_sim_session_fence_until(s, 0, EFS_SIM_FENCE_ACTIVE) == EFS_OK,
          "active");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "c", &b) == EFS_OK &&
              b,
          "new epoch");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "re-PUT new epoch");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_OK, "publish new");
    memset(got, 0, sizeof(got));
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK, "read");
    CHECK(memcmp(src, got, sizeof(src)) == 0, "bytes");
    efs_sim_free(s);
}

static void test_i19_open_unlinked(void)
{
    struct efs_sim *s = mk(22);
    efs_ino_t ino = 0, g = 0;
    uint32_t nlink = 99;
    uint64_t gen = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "o", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_open(s, 0, ino) == EFS_OK, "open");
    CHECK(efs_sim_unlink(s, 0, EFS_ROOT_INO, "o") == EFS_OK, "unlink");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "o", &g) == EFS_ERR_NOT_FOUND,
          "name gone");
    CHECK(efs_sim_inode_nlink(s, ino, &nlink, &gen) == EFS_OK && nlink == 0,
          "orphan");
    CHECK(efs_sim_reclaim(s, ino) == EFS_ERR_BUSY, "lease holds");
    CHECK(efs_sim_close(s, 0, ino) == EFS_OK, "close");
    CHECK(efs_sim_reclaim(s, ino) == EFS_OK, "reclaim");
    CHECK(efs_sim_inode_nlink(s, ino, &nlink, &gen) == EFS_ERR_NOT_FOUND,
          "reclaimed");
    efs_sim_free(s);
}

static void test_i23_barrier_holds_leases(void)
{
    struct efs_sim *s = mk(23);
    efs_ino_t ino = 0;
    uint32_t nlink = 99;
    uint64_t gen = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "p", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_open(s, 0, ino) == EFS_OK, "open");
    CHECK(efs_sim_unlink(s, 0, EFS_ROOT_INO, "p") == EFS_OK, "unlink");
    CHECK(efs_sim_session_fence_until(s, 0, EFS_SIM_FENCE_ACK) == EFS_OK, "ack");
    CHECK(efs_sim_reclaim(s, ino) == EFS_ERR_BUSY, "before ACTIVE");
    CHECK(efs_sim_close(s, 0, ino) == EFS_ERR_STALE, "close old epoch");
    CHECK(efs_sim_session_fence_until(s, 0, EFS_SIM_FENCE_ACTIVE) == EFS_OK,
          "active drops");
    CHECK(efs_sim_reclaim(s, ino) == EFS_OK, "reclaim after barrier");
    CHECK(efs_sim_inode_nlink(s, ino, &nlink, &gen) == EFS_ERR_NOT_FOUND,
          "gone");
    efs_sim_free(s);
}

static void test_i20_cas(void)
{
    struct efs_sim *s = mk(31);
    efs_ino_t ino = 0;
    uint8_t a[32], b[32], got[32];
    struct efs_frag_id loser;
    uint32_t i;

    CHECK(s, "mk");
    for (i = 0; i < sizeof(a); i++) {
        a[i] = (uint8_t)(0x10 + i);
        b[i] = (uint8_t)(0x80 + i);
    }
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "w", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, a, sizeof(a), -1) == EFS_OK, "put A");
    CHECK(efs_sim_put_stripe(s, 1, ino, 0, b, sizeof(b), -1) == EFS_OK, "put B");
    CHECK(efs_sim_publish_cas(s, 0, ino, 0, sizeof(a), 0, 0, 0) == EFS_OK,
          "A wins");
    CHECK(efs_sim_publish_cas(s, 1, ino, 0, sizeof(b), 0, 0, 0) == EFS_ERR_STALE,
          "I20 B loses");
    memset(got, 0, sizeof(got));
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK, "read");
    CHECK(memcmp(got, a, sizeof(a)) == 0, "winner bytes");
    CHECK(efs_sim_frag_id(s, 1, ino, 0, 0, 0, 0, &loser) == EFS_OK, "loser id");
    CHECK(efs_sim_frag_present(s, &loser) == 1, "orphan remains");
    efs_sim_free(s);
}

static void test_i13_fileid(void)
{
    struct efs_sim *s = mk(37);
    efs_ino_t ino = 0;
    uint8_t src[16], got[16];
    struct efs_frag_id stale, live;

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "g", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe_as(s, 0, ino, 0, src, sizeof(src), -1, 99, 0) ==
              EFS_OK,
          "PUT stale gen");
    CHECK(efs_sim_frag_id(s, 0, ino, 0, 0, 99, 0, &stale) == EFS_OK, "stale id");
    CHECK(efs_sim_frag_present(s, &stale) == 1, "on disk");
    CHECK(efs_sim_frag_id(s, 0, ino, 0, 0, 0, 0, &live) == EFS_OK, "live id");
    CHECK(efs_sim_frag_present(s, &live) == 0, "not live FileID");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_ERR_IO, "no live frags");
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_ERR_NOT_FOUND,
          "unpublished");
    efs_sim_free(s);
}

static void test_i22_epoch(void)
{
    struct efs_sim *s = mk(41);
    efs_ino_t ino = 0;
    uint8_t src[24], got[24];

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "t", &ino) == EFS_OK,
          "create");
    CHECK(efs_sim_epoch_fence(s, ino) == EFS_OK, "fence");
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "PUT after fence");
    CHECK(efs_sim_publish_cas(s, 0, ino, 0, sizeof(src), 0, 0, 0) ==
              EFS_ERR_STALE,
          "old epoch");
    CHECK(efs_sim_publish(s, 0, ino, 0, sizeof(src)) == EFS_OK, "live epoch");
    memset(got, 0, sizeof(got));
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK, "read");
    CHECK(memcmp(src, got, sizeof(src)) == 0, "bytes");
    efs_sim_free(s);
}

static const char *hashed_nm(efs_ino_t parent)
{
    static char buf[16];
    int i;

    for (i = 0; i < 4096; i++) {
        snprintf(buf, sizeof(buf), "h%d", i);
        if (efs_kv_dir_lane(buf) != 0 &&
            efs_kv_dentry_shard(parent, buf, EFS_META_LAYOUT_HASHED) !=
                efs_kv_inode_shard(parent))
            return buf;
    }
    return "h1";
}

static void test_i8_spread(void)
{
    struct efs_sim *s = mk(91);
    efs_ino_t foo = 0, bar = 0, g = 0;
    uint8_t layout = 99;
    const char *nm;
    int rc;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "foo", &foo) ==
              EFS_OK,
          "foo");
    CHECK(efs_sim_dir_begin_split(s, EFS_ROOT_INO) == EFS_OK, "split");
    CHECK(efs_sim_dir_layout(s, EFS_ROOT_INO, &layout, NULL) == EFS_OK &&
              layout == EFS_META_LAYOUT_SPLITTING,
          "SPLITTING");
    CHECK(efs_sim_unlink(s, 0, EFS_ROOT_INO, "foo") == EFS_OK, "unlink");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "foo", &g) == EFS_ERR_NOT_FOUND,
          "I8");
    nm = hashed_nm(EFS_ROOT_INO);
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, nm, &bar) ==
              EFS_OK &&
              bar,
          "hashed create");
    CHECK((bar & 0xFFF) ==
              efs_kv_dentry_shard(EFS_ROOT_INO, nm, EFS_META_LAYOUT_HASHED),
          "dentry shard");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_OK && g == bar,
          "lookup");
    while ((rc = efs_sim_dir_migrate(s, EFS_ROOT_INO)) == EFS_OK)
        ;
    CHECK(rc == EFS_ERR_NOT_FOUND, "migrated");
    CHECK(efs_sim_dir_finish_hashed(s, EFS_ROOT_INO) == EFS_OK, "HASHED");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, nm, &g) == EFS_OK && g == bar,
          "peer");
    efs_sim_free(s);
}

static int hashed_pair(efs_ino_t parent, char *a, char *b)
{
    uint8_t la = 0xff;
    int i;

    a[0] = b[0] = 0;
    for (i = 0; i < 8192; i++) {
        char buf[16];
        uint8_t lane;
        uint32_t sh;

        snprintf(buf, sizeof(buf), "p%d", i);
        lane = efs_kv_dir_lane(buf);
        sh = efs_kv_dentry_shard(parent, buf, EFS_META_LAYOUT_HASHED);
        if (lane == 0 || sh == efs_kv_inode_shard(parent))
            continue;
        if (la == 0xff) {
            snprintf(a, 16, "%s", buf);
            la = lane;
        } else if (lane != la) {
            snprintf(b, 16, "%s", buf);
            return 0;
        }
    }
    return -1;
}

static void test_hashed_dir_stat(void)
{
    struct efs_sim *s = mk(113);
    efs_ino_t fa = 0, fb = 0;
    struct efs_meta_stat st;
    struct efs_meta_setattr sa;
    struct efs_meta_utimens u;
    uint8_t layout = 99;
    char na[16], nb[16];
    uint64_t born, t1, t2;
    int rc;

    CHECK(s, "mk");
    CHECK(hashed_pair(EFS_ROOT_INO, na, nb) == 0, "two lanes");
    CHECK(efs_sim_dir_begin_split(s, EFS_ROOT_INO) == EFS_OK, "split");
    while ((rc = efs_sim_dir_migrate(s, EFS_ROOT_INO)) == EFS_OK)
        ;
    CHECK(rc == EFS_ERR_NOT_FOUND, "empty migrate");
    CHECK(efs_sim_dir_finish_hashed(s, EFS_ROOT_INO) == EFS_OK, "HASHED");
    CHECK(efs_sim_dir_layout(s, EFS_ROOT_INO, &layout, NULL) == EFS_OK &&
              layout == EFS_META_LAYOUT_HASHED,
          "layout");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK, "stat empty");
    born = st.mtime;

    CHECK(efs_sim_clock_step(s, 10) == EFS_OK, "tick");
    t1 = efs_sim_now(s);
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, na, &fa) == EFS_OK,
          "create a");
    CHECK(efs_sim_clock_step(s, 10) == EFS_OK, "tick");
    t2 = efs_sim_now(s);
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, nb, &fb) == EFS_OK,
          "create b");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK, "stat");
    CHECK(st.mtime == t2 && st.ctime == t2, "MAX over dir lanes");
    CHECK(st.lanes >= 2, "both lanes");
    CHECK(t2 > t1 && t1 > born, "creates used the simulated clock");

    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0700;
    CHECK(efs_sim_clock_step(s, 5) == EFS_OK, "tick");
    CHECK(efs_sim_setattr(s, 0, EFS_ROOT_INO, &sa) == EFS_OK, "chmod dir");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK, "stat");
    CHECK((st.mode & 07777u) == 0700, "mode");
    CHECK(st.mtime == t2, "chmod left mtime on the lanes");

    memset(&u, 0, sizeof(u));
    u.mask = EFS_META_SET_MTIME;
    u.mtime = born;
    CHECK(efs_sim_utimens(s, 0, EFS_ROOT_INO, &u) == EFS_OK, "utimens dir");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK && st.mtime == born,
          "dir utimens wins over lanes");

    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash");
    CHECK(efs_sim_getattr(s, EFS_ROOT_INO, &st) == EFS_OK && st.mtime == born,
          "stat after crash");
    CHECK((st.mode & 07777u) == 0700, "mode after crash");
    efs_sim_free(s);
}

static void test_lock_conflict_fence(void)
{
    struct efs_sim *s = mk(93);
    efs_ino_t ino = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "lk", &ino) ==
              EFS_OK,
          "create");
    CHECK(efs_sim_lock(s, 0, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
                       EFS_LOCK_PROC, 1) == EFS_OK,
          "A");
    CHECK(efs_sim_lock(s, 1, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
                       EFS_LOCK_PROC, 2) == EFS_ERR_AGAIN,
          "conflict");
    CHECK(efs_sim_lock(s, 1, ino, EFS_LOCK_FLOCK, EFS_LOCK_EX, 0, ~(uint64_t)0,
                       EFS_LOCK_OFD, 9) == EFS_OK,
          "flock");
    CHECK(efs_sim_lockw(s, 1, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
                        EFS_LOCK_PROC, 2) == EFS_ERR_BUSY,
          "queued");
    CHECK(efs_sim_session_fence(s, 1) == EFS_OK, "fence B");
    CHECK(efs_sim_unlock(s, 0, ino, EFS_LOCK_FCNTL, 0, 10, EFS_LOCK_PROC, 1) ==
              EFS_OK,
          "unlock");
    CHECK(efs_sim_lock(s, 0, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
                       EFS_LOCK_PROC, 1) == EFS_OK,
          "A regrant; fenced waiter never granted");
    efs_sim_free(s);
}

static int dir_has(struct efs_sim *s, efs_ino_t dir, const char *name)
{
    struct efs_meta_dir_cursor cur;
    struct efs_meta_dir_ent ents[16];
    uint32_t n = 0, i;

    memset(&cur, 0, sizeof(cur));
    do {
        n = 0;
        if (efs_sim_readdir(s, dir, &cur, ents, 16, &n) != EFS_OK)
            return 0;
        for (i = 0; i < n; i++)
            if (strcmp(ents[i].name, name) == 0)
                return 1;
    } while (!cur.done);
    return 0;
}

static void test_single_shard_ops(void)
{
    struct efs_sim *s = mk(101);
    efs_ino_t f = 0, d = 0, nested = 0, leaf = 0;
    struct efs_meta_stat st;
    struct efs_meta_setattr sa;
    struct efs_meta_path_hop hops[8];
    uint32_t n = 0;
    uint8_t src[64];
    uint64_t mt;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &f) == EFS_OK,
          "create");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == 0 && st.nlink == 1,
          "stat empty");
    CHECK((st.mode & 07777u) == 0644 && st.uid == 1000, "create attrs");

    fill(src, sizeof(src));
    CHECK(efs_sim_put_stripe(s, 0, f, 0, src, sizeof(src), -1) == EFS_OK, "put");
    CHECK(efs_sim_publish(s, 0, f, 0, sizeof(src)) == EFS_OK, "publish");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == sizeof(src), "size");
    mt = st.mtime;

    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0600;
    CHECK(efs_sim_setattr(s, 0, f, &sa) == EFS_OK, "chmod");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK, "stat");
    CHECK((st.mode & 07777u) == 0600 && S_ISREG(st.mode), "mode, type held");
    CHECK(st.mtime == mt, "chmod does not move mtime");
    CHECK(st.size == sizeof(src), "size held");

    CHECK(dir_has(s, EFS_ROOT_INO, "f"), "readdir sees f");

    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, "d", &d) == EFS_OK && d, "mkdir");
    CHECK(efs_sim_mkdir(s, 0, d, "n", &nested) == EFS_OK && nested, "nested");
    CHECK(efs_sim_create(s, 0, nested, S_IFREG | 0644, "leaf", &leaf) == EFS_OK,
          "leaf");
    CHECK(dir_has(s, EFS_ROOT_INO, "d"), "readdir sees d");
    CHECK(efs_sim_lookup_path(s, 0, "/d/n/leaf", hops, 8, &n) == EFS_OK && n == 3,
          "path");
    CHECK(hops[0].ino == d && S_ISDIR(hops[0].mode), "hop a");
    CHECK(hops[1].ino == nested && S_ISDIR(hops[1].mode), "hop n");
    CHECK(hops[2].ino == leaf && S_ISREG(hops[2].mode), "hop leaf");

    /* A lost leader must not change a linearizable read: the remaining
     * majority still has the committed setattr and the nested mkdir. */
    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash leader");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && (st.mode & 07777u) == 0600,
          "stat after crash");
    CHECK(efs_sim_lookup_path(s, 0, "/d/n/leaf", hops, 8, &n) == EFS_OK &&
              n == 3 && hops[2].ino == leaf,
          "path after crash");
    CHECK(efs_sim_restart(s, EFS_SIM_META) == EFS_OK, "restart");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == sizeof(src),
          "stat after restart");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_utimens_fence(void)
{
    struct efs_sim *s = mk(103);
    efs_ino_t f = 0;
    struct efs_meta_stat st;
    struct efs_meta_utimens u;
    struct efs_meta_setattr sa;
    uint8_t src[64];
    uint64_t write_mt, after;

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_clock_step(s, 50) == EFS_OK, "tick");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &f) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, f, 0, src, sizeof(src), -1) == EFS_OK, "put0");
    CHECK(efs_sim_publish(s, 0, f, 0, sizeof(src)) == EFS_OK, "pub0");
    CHECK(efs_sim_put_stripe(s, 0, f, 1, src, sizeof(src), -1) == EFS_OK, "put1");
    CHECK(efs_sim_publish(s, 0, f, 1, (uint64_t)EFS_SIM_CHUNK + sizeof(src)) ==
              EFS_OK,
          "pub1");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK, "stat");
    write_mt = st.mtime;
    CHECK(write_mt > 1, "writes stamped a real time");

    memset(&u, 0, sizeof(u));
    u.mask = EFS_META_SET_MTIME | EFS_META_SET_ATIME;
    u.mtime = 1;
    u.atime = 2;
    CHECK(efs_sim_utimens(s, 0, f, &u) == EFS_OK, "utimens");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK, "stat");
    CHECK(st.mtime == 1 && st.atime == 2, "backwards honored");

    memset(&sa, 0, sizeof(sa));
    sa.mask = EFS_META_SET_MODE;
    sa.mode = 0600;
    CHECK(efs_sim_setattr(s, 0, f, &sa) == EFS_OK, "chmod");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.mtime == 1,
          "chmod left utimens mtime");

    u.expect_gen = st.generation + 1;
    CHECK(efs_sim_utimens(s, 0, f, &u) == EFS_ERR_STALE, "stale handle");

    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.mtime == 1, "after crash");
    CHECK(efs_sim_restart(s, EFS_SIM_META) == EFS_OK, "restart");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.mtime == 1, "after restart");

    CHECK(efs_sim_clock_step(s, 10) == EFS_OK, "tick");
    after = efs_sim_now(s);
    CHECK(efs_sim_put_stripe(s, 0, f, 64, src, sizeof(src), -1) == EFS_OK,
          "put after");
    CHECK(efs_sim_publish(s, 0, f, 64,
                          64ull * (uint64_t)EFS_SIM_CHUNK + sizeof(src)) ==
              EFS_OK,
          "pub after");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK, "stat");
    CHECK(st.mtime >= after, "write after fence is visible");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_lock_deadlock(void)
{
    struct efs_sim *s = mk(97);
    efs_ino_t ino = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "dl", &ino) ==
              EFS_OK,
          "create");
    CHECK(efs_sim_lock(s, 0, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
                       EFS_LOCK_PROC, 1) == EFS_OK,
          "A");
    CHECK(efs_sim_lock(s, 1, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 10, 20,
                       EFS_LOCK_PROC, 2) == EFS_OK,
          "B");
    CHECK(efs_sim_lockw(s, 0, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 10, 20,
                        EFS_LOCK_PROC, 1) == EFS_ERR_BUSY,
          "A waits");
    CHECK(efs_sim_lockw(s, 1, ino, EFS_LOCK_FCNTL, EFS_LOCK_EX, 0, 10,
                        EFS_LOCK_PROC, 2) == EFS_ERR_DEADLK,
          "EDEADLK");
    efs_sim_free(s);
}

static void test_truncate_range_del(void)
{
    struct efs_sim *s = mk(107);
    efs_ino_t f = 0;
    struct efs_meta_stat st;
    uint8_t src[64], got[64];

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &f) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, f, 0, src, sizeof(src), -1) == EFS_OK, "put0");
    CHECK(efs_sim_publish(s, 0, f, 0, sizeof(src)) == EFS_OK, "pub0");
    CHECK(efs_sim_put_stripe(s, 0, f, 64, src, sizeof(src), -1) == EFS_OK, "put64");
    CHECK(efs_sim_publish(s, 0, f, 64, (uint64_t)EFS_SIM_CHUNK + sizeof(src)) ==
              EFS_OK,
          "pub64");
    CHECK(efs_sim_read_chunk(s, f, 0, got, sizeof(got)) == EFS_OK, "read0");
    CHECK(efs_sim_read_chunk(s, f, 64, got, sizeof(got)) == EFS_OK, "read64");

    CHECK(efs_sim_truncate(s, 0, f, 0, NULL) == EFS_OK, "truncate zero");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == 0, "size zero");
    CHECK(efs_sim_read_chunk(s, f, 0, got, sizeof(got)) == EFS_ERR_NOT_FOUND,
          "chunk0 gone");
    CHECK(efs_sim_read_chunk(s, f, 64, got, sizeof(got)) == EFS_ERR_NOT_FOUND,
          "chunk64 gone");

    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash");
    CHECK(efs_sim_restart(s, EFS_SIM_META) == EFS_OK, "restart");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == 0, "durable size");
    CHECK(efs_sim_read_chunk(s, f, 64, got, sizeof(got)) == EFS_ERR_NOT_FOUND,
          "still gone");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_append_reserve(void)
{
    struct efs_sim *s = mk(108);
    struct efs_opid op;
    struct efs_meta_stat st;
    efs_ino_t f = 0;
    uint64_t off = 0, off2 = 0;
    uint8_t src[64];

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &f) == EFS_OK,
          "create");
    CHECK(efs_sim_put_stripe(s, 0, f, 0, src, sizeof(src), -1) == EFS_OK, "put");
    CHECK(efs_sim_publish(s, 0, f, 0, sizeof(src)) == EFS_OK, "pwrite");
    CHECK(efs_sim_append_reserve(s, 0, f, 64, &off) == EFS_OK && off == 64,
          "reserve after pwrite");
    CHECK(efs_sim_opid_forget(s, 0) == EFS_OK, "forget ram");
    efs_sim_opid_for(s, 0, 1, &op);
    CHECK(efs_sim_append_reserve_op(s, 0, &op, f, 64, &off2) == EFS_OK &&
              off2 == 64,
          "i16 durable");
    CHECK(efs_sim_put_stripe(s, 0, f, 1, src, sizeof(src), -1) == EFS_OK, "put1");
    CHECK(efs_sim_publish(s, 0, f, 1, 200) == EFS_ERR_BUSY, "past bar");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == 64, "frontier cap");
    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash");
    CHECK(efs_sim_restart(s, EFS_SIM_META) == EFS_OK, "restart");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == 64, "durable cap");
    CHECK(efs_sim_append_resolve(s, 0, f, off, EFS_META_APPEND_COMPLETED) ==
              EFS_OK,
          "resolve");
    CHECK(efs_sim_getattr(s, f, &st) == EFS_OK && st.size == 128, "drained");
    CHECK(efs_sim_append_resolve(s, 0, f, off, EFS_META_APPEND_COMPLETED) ==
              EFS_OK,
          "resolve replay");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_link_i17(void)
{
    struct efs_sim *s = mk(81);
    efs_ino_t f = 0, d = 0, g = 0;
    struct efs_txid t;
    const char *nm;
    uint32_t nlink = 0;

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "f", &f) == EFS_OK &&
              f,
          "file");
    nm = scatter(EFS_ROOT_INO);
    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, nm, &d) == EFS_OK && d, "dir");
    CHECK(efs_kv_inode_shard(d) != efs_kv_inode_shard(f), "two shards");
    CHECK(efs_sim_link_until(s, EFS_ROOT_INO, "f", d, "alias", &t,
                             EFS_SIM_TXN_PREPARE) == EFS_OK,
          "prepare");
    CHECK(efs_sim_lookup(s, 0, d, "alias", &g) == EFS_ERR_NOT_FOUND,
          "I17 no half-apply");
    CHECK(efs_sim_txn_finish(s, &t, 0) == EFS_OK, "abort");
    CHECK(efs_sim_inode_nlink(s, f, &nlink, NULL) == EFS_OK && nlink == 1,
          "nlink unchanged");
    CHECK(efs_sim_link(s, 0, EFS_ROOT_INO, "f", d, "alias") == EFS_OK, "link");
    CHECK(efs_sim_lookup(s, 1, d, "alias", &g) == EFS_OK && g == f, "visible");
    CHECK(efs_sim_inode_nlink(s, f, &nlink, NULL) == EFS_OK && nlink == 2,
          "nlink 2");
    CHECK(efs_sim_unlink(s, 0, EFS_ROOT_INO, "f") == EFS_OK, "unlink orig");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "f", &g) == EFS_ERR_NOT_FOUND,
          "orig gone");
    CHECK(efs_sim_lookup(s, 0, d, "alias", &g) == EFS_OK && g == f, "alias");
    CHECK(efs_sim_inode_nlink(s, f, &nlink, NULL) == EFS_OK && nlink == 1,
          "nlink 1");
    CHECK(efs_sim_unlink(s, 0, d, "alias") == EFS_OK, "last cross-shard");
    CHECK(efs_sim_lookup(s, 0, d, "alias", &g) == EFS_ERR_NOT_FOUND, "gone");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_rmdir_rename(void)
{
    struct efs_sim *s = mk(83);
    efs_ino_t d = 0, f = 0, g = 0, d2 = 0;
    struct efs_txid t;
    const char *nm;

    CHECK(s, "mk");
    nm = scatter(EFS_ROOT_INO);
    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, nm, &d) == EFS_OK && d, "mkdir");
    CHECK(efs_sim_rmdir_until(s, EFS_ROOT_INO, nm, &t, EFS_SIM_TXN_PREPARE) ==
              EFS_OK,
          "rmdir prepare");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_OK && g == d,
          "I17 still there");
    CHECK(efs_sim_txn_finish(s, &t, 0) == EFS_OK, "abort");
    CHECK(efs_sim_create(s, 0, d, S_IFREG | 0644, "x", &f) == EFS_OK, "child");
    CHECK(efs_sim_rmdir(s, 0, EFS_ROOT_INO, nm) == EFS_ERR_NOT_EMPTY, "not empty");
    CHECK(efs_sim_unlink(s, 0, d, "x") == EFS_OK, "unlink child");
    CHECK(efs_sim_rmdir(s, 0, EFS_ROOT_INO, nm) == EFS_OK, "rmdir");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_ERR_NOT_FOUND,
          "dir gone");

    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "a", &f) == EFS_OK,
          "a");
    CHECK(efs_sim_rename(s, 0, EFS_ROOT_INO, "a", EFS_ROOT_INO, "b") == EFS_OK,
          "same-dir");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "a", &g) == EFS_ERR_NOT_FOUND,
          "old");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "b", &g) == EFS_OK && g == f,
          "new");
    nm = scatter(EFS_ROOT_INO);
    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, nm, &d2) == EFS_OK, "dest dir");
    CHECK(efs_sim_rename_until(s, EFS_ROOT_INO, "b", d2, "c", &t,
                               EFS_SIM_TXN_DECISION) == EFS_OK,
          "cross decide");
    CHECK(efs_sim_lookup(s, 0, d2, "c", &g) == EFS_OK && g == f,
          "visible at decision");
    CHECK(efs_sim_txn_finish(s, &t, 1) == EFS_OK, "resolve");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, "b", &g) == EFS_ERR_NOT_FOUND,
          "src gone");
    CHECK(efs_sim_rename(s, 0, EFS_ROOT_INO, nm, EFS_ROOT_INO, "moved") == EFS_OK,
          "dir rename");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "moved", &g) == EFS_OK && g == d2,
          "moved");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, nm, &g) == EFS_ERR_NOT_FOUND,
          "old dir name");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

static void test_dir_rename(void)
{
    struct efs_sim *s = mk(85);
    efs_ino_t a = 0, b = 0, g = 0;
    struct efs_txid t, t2;
    struct efs_meta_stat st;
    char na[16], nb[16];

    CHECK(s, "mk");
    snprintf(na, sizeof(na), "%s", scatter_nth(EFS_ROOT_INO, 0));
    snprintf(nb, sizeof(nb), "%s", scatter_nth(EFS_ROOT_INO, 1));
    CHECK(strcmp(na, nb) != 0, "two names");
    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, na, &a) == EFS_OK && a, "mkdir a");
    CHECK(efs_sim_mkdir(s, 0, EFS_ROOT_INO, nb, &b) == EFS_OK && b, "mkdir b");
    CHECK(efs_sim_rename(s, 0, EFS_ROOT_INO, na, a, "x") == EFS_ERR_INVAL,
          "into self");
    CHECK(efs_sim_rename_until(s, EFS_ROOT_INO, na, b, "a", &t,
                               EFS_SIM_TXN_PREPARE) == EFS_OK,
          "prepare a into b");
    CHECK(efs_sim_lookup(s, 0, b, "a", &g) == EFS_ERR_NOT_FOUND, "I17");
    CHECK(efs_sim_rename_until(s, EFS_ROOT_INO, nb, a, "b", &t2,
                               EFS_SIM_TXN_PREPARE) == EFS_ERR_BUSY,
          "cycle prepare");
    CHECK(efs_sim_txn_finish(s, &t, 1) == EFS_OK, "commit a into b");
    CHECK(efs_sim_lookup(s, 0, b, "a", &g) == EFS_OK && g == a, "nested");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, na, &g) == EFS_ERR_NOT_FOUND,
          "a gone");
    CHECK(efs_sim_getattr(s, b, &st) == EFS_OK && st.nlink == 3, "b nlink");
    CHECK(efs_sim_rename(s, 0, EFS_ROOT_INO, nb, a, "b") == EFS_ERR_INVAL,
          "cycle committed");
    CHECK(efs_sim_check(s) == EFS_OK, "check");
    efs_sim_free(s);
}

int main(void)
{
    test_replay();
    test_table();
    test_export_mkfs();
    test_restart();
    test_publish_roundtrip();
    test_orphan();
    test_i14();
    test_i25();
    test_faults();
    test_opid_window();
    test_i16();
    test_i1_one_leader();
    test_i3_term_fence();
    test_i4_no_quorum_commit();
    test_readindex();
    test_l8_desired_placement();
    test_mkdir_i17();
    test_mkdir_crash_after_prepare();
    test_mkdir_visible_at_decision();
    test_link_i17();
    test_rmdir_rename();
    test_dir_rename();
    test_i23_session_fence();
    test_i19_open_unlinked();
    test_i23_barrier_holds_leases();
    test_i20_cas();
    test_i13_fileid();
    test_i22_epoch();
    test_i8_spread();
    test_hashed_dir_stat();
    test_single_shard_ops();
    test_utimens_fence();
    test_truncate_range_del();
    test_append_reserve();
    test_lock_conflict_fence();
    test_lock_deadlock();
    if (failures) {
        fprintf(stderr, "test_sim: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_sim: OK\n");
    return 0;
}
