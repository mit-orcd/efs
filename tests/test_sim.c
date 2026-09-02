/* Deterministic simulator: current efs_export SM + mem store/kv/loop.
 * Same seed must replay the same history. */
#include "efs/sim.h"
#include "efs/opid.h"
#include "efs/common.h"
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

    CHECK(s, "mk");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "keep", &a) == EFS_OK,
          "create");
    CHECK(efs_sim_crash(s, EFS_SIM_META) == EFS_OK, "crash");
    CHECK(efs_sim_lookup(s, 0, EFS_ROOT_INO, "keep", &g) == EFS_ERR_BUSY, "down");
    CHECK(efs_sim_restart(s, EFS_SIM_META) == EFS_OK, "restart");
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
    CHECK(efs_sim_partition(p, EFS_SIM_META, 1) == EFS_OK, "part on");
    CHECK(efs_sim_create(p, 0, EFS_ROOT_INO, S_IFREG | 0644, "y", &g) == EFS_ERR_BUSY,
          "partitioned");
    CHECK(efs_sim_partition(p, EFS_SIM_META, 0) == EFS_OK, "part off");
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
    if (failures) {
        fprintf(stderr, "test_sim: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_sim: OK\n");
    return 0;
}
