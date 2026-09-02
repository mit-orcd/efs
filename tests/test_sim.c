/* Deterministic simulator: RF=3 Raft + KV apply SM + mem store/loop.
 * Same seed must replay the same history. */
#include "efs/sim.h"
#include "efs/opid.h"
#include "efs/common.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
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
    CHECK(efs_sim_publish(s, ino, 0, sizeof(src)) == EFS_OK, "publish");
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK, "read");
    CHECK(memcmp(src, got, sizeof(src)) == 0, "bytes");
    efs_sim_free(s);
}

static void test_orphan(void)
{
    struct efs_sim *s = mk(13);
    efs_ino_t ino = 0;
    uint8_t src[32], got[32];
    struct efs_frag_id id = { .export_id = 1, .chunk_index = 0,
                              .fragment_index = 0 };

    CHECK(s, "mk");
    fill(src, sizeof(src));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "o", &ino) == EFS_OK,
          "create");
    id.ino = ino;
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "put");
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
    CHECK(efs_sim_publish(s, ino, 0, sizeof(src)) == EFS_ERR_IO, "I14");
    efs_sim_free(s);
}

static void test_i25(void)
{
    struct efs_sim *s = mk(19);
    efs_ino_t ino = 0;
    uint8_t src[48], got[48];
    struct efs_frag_id id = { .export_id = 1, .chunk_index = 0,
                              .fragment_index = 2 };

    CHECK(s, "mk");
    fill(src, sizeof(src));
    memset(got, 0, sizeof(got));
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "c", &ino) == EFS_OK,
          "create");
    id.ino = ino;
    CHECK(efs_sim_put_stripe(s, 0, ino, 0, src, sizeof(src), -1) == EFS_OK,
          "put");
    CHECK(efs_sim_publish(s, ino, 0, sizeof(src)) == EFS_OK, "publish");
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

    CHECK(s, "drop sim");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "x", &g) == EFS_ERR_AGAIN,
          "always-drop");
    CHECK(g == 0, "no ino");
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

static const char *scatter(efs_ino_t parent)
{
    static char buf[16];
    int i;

    for (i = 0; i < 4096; i++) {
        snprintf(buf, sizeof(buf), "d%d", i);
        if (efs_kv_mkdir_shard(parent, buf, 0) != efs_kv_inode_shard(parent))
            return buf;
    }
    return "d0";
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

int main(void)
{
    test_replay();
    test_table();
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
    if (failures) {
        fprintf(stderr, "test_sim: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_sim: OK\n");
    return 0;
}
