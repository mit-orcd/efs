/* Deterministic fault matrix on the in-process simulator.
 *
 * This is not disk and it is not FUSE. efs_sim_read_chunk reads a crashed
 * server's store directly, so a passing sim read does not prove a live
 * client can reconstruct while that process is gone. The integration
 * harness in harness.py is that check.
 *
 * Repair is not implemented. A readable chunk with a damaged fragment is
 * reported as a gap for "fully protected", not as a pass.
 */
#include "efs/sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
static int gaps;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static struct efs_sim *mk(uint64_t seed)
{
    struct efs_sim_cfg cfg = {
        .seed = seed, .nservers = 3, .nclients = 2
    };
    return efs_sim_new(&cfg);
}

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + (i * 17u) + 1u);
}

static void emit(const char *cas, const char *status, const char *detail)
{
    printf("{\"layer\":\"sim\",\"case\":\"%s\",\"status\":\"%s\","
           "\"detail\":\"%s\",\"completion\":\"sim_publish\"}\n",
           cas, status, detail);
}

static int mark;

static void begin(void)
{
    mark = failures;
}

static void finish(const char *cas, const char *detail)
{
    if (failures == mark)
        emit(cas, "PASS", detail);
    else
        emit(cas, "FAIL", detail);
}

static int publish_file(struct efs_sim *s, const char *name, const uint8_t *src,
                        uint32_t n, efs_ino_t *ino)
{
    int rc;

    rc = efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, name, ino);
    if (rc != EFS_OK)
        return rc;
    rc = efs_sim_put_stripe(s, 0, *ino, 0, src, n, -1);
    if (rc != EFS_OK)
        return rc;
    return efs_sim_publish(s, 0, *ino, 0, n);
}

static int frag_of(struct efs_sim *s, efs_ino_t ino, uint32_t fi,
                   struct efs_frag_id *id)
{
    return efs_sim_frag_id(s, 0, ino, 0, fi, 0, 0, id);
}

/* Kill each server after a published chunk. Metadata lookup still
 * succeeds on the remaining quorum. A new publication must not succeed
 * while one fragment holder is down. */
static void case_one_server_lost(void)
{
    struct efs_sim *s = mk(101);
    efs_ino_t ino = 0, neu = 0, looked = 0;
    uint8_t src[64], got[64];
    int i, lid;
    char name[16];

    begin();
    CHECK(s, "mk");
    if (!s) {
        finish("one_server_lost", "simulator did not start");
        return;
    }
    fill(src, sizeof(src), 0x31);
    CHECK(publish_file(s, "kept", src, sizeof(src), &ino) == EFS_OK, "publish");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    for (i = 0; i < 3; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        CHECK(efs_sim_crash(s, i) == EFS_OK, "crash");
        memset(got, 0x5a, sizeof(got));
        CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK,
              "read");
        CHECK(memcmp(src, got, sizeof(src)) == 0, "bytes");
        CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, "kept", &looked) == EFS_OK &&
                  looked == ino,
              "lookup on other client");
        CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, name, &neu) ==
                  EFS_OK,
              "create under one metadata voter down");
        CHECK(efs_sim_put_stripe(s, 0, neu, 0, src, sizeof(src), -1) != EFS_OK,
              "put must not succeed with a holder down");
        CHECK(efs_sim_publish(s, 0, neu, 0, sizeof(src)) != EFS_OK,
              "publish must not succeed with a holder down");
        CHECK(efs_sim_restart(s, i) == EFS_OK, "restart");
        memset(got, 0x5a, sizeof(got));
        CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK &&
                  memcmp(src, got, sizeof(src)) == 0,
              "read after restart");
    }
    CHECK(lid >= 0 && lid < 3, "leader was one of the crashed servers");
    efs_sim_free(s);
    finish("one_server_lost",
           "readable with one voter down; new publication rejected; "
           "sim read still opens the crashed server store");
}

static void case_restart(void)
{
    struct efs_sim *s = mk(102);
    efs_ino_t ino = 0, unpublished = 0, looked = 0;
    uint8_t src[64], got[64];
    int lid;

    begin();
    CHECK(s, "mk");
    if (!s) {
        finish("restart", "simulator did not start");
        return;
    }
    fill(src, sizeof(src), 0x42);
    CHECK(publish_file(s, "dur", src, sizeof(src), &ino) == EFS_OK, "publish");
    CHECK(efs_sim_create(s, 0, EFS_ROOT_INO, S_IFREG | 0644, "open",
                         &unpublished) == EFS_OK,
          "create unpublished");
    CHECK(efs_sim_put_stripe(s, 0, unpublished, 0, src, sizeof(src), -1) ==
              EFS_OK,
          "put unpublished");
    lid = efs_sim_meta_leader(s);
    CHECK(lid >= 0, "leader");
    CHECK(efs_sim_crash(s, lid) == EFS_OK, "crash");
    CHECK(efs_sim_restart(s, lid) == EFS_OK, "restart");
    memset(got, 0x5a, sizeof(got));
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK &&
              memcmp(src, got, sizeof(src)) == 0,
          "published bytes survived");
    CHECK(efs_sim_lookup(s, 1, EFS_ROOT_INO, "dur", &looked) == EFS_OK &&
              looked == ino,
          "name survived");
    memset(got, 0x5a, sizeof(got));
    CHECK(efs_sim_read_chunk(s, unpublished, 0, got, sizeof(got)) ==
              EFS_ERR_NOT_FOUND,
          "unpublished chunk is not durable");
    efs_sim_free(s);
    finish("restart",
           "published bytes and name survive same-storage restart; "
           "unpublished put does not");
}

static void case_one_corrupt(void)
{
    int fi;

    begin();
    for (fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        struct efs_sim *s = mk(110 + (uint64_t)fi);
        efs_ino_t ino = 0;
        struct efs_frag_id id;
        uint8_t src[64], got[64];

        CHECK(s, "mk");
        if (!s)
            continue;
        fill(src, sizeof(src), (uint8_t)(0x50 + fi));
        CHECK(publish_file(s, "c", src, sizeof(src), &ino) == EFS_OK, "publish");
        CHECK(frag_of(s, ino, (uint32_t)fi, &id) == EFS_OK, "id");
        CHECK(efs_sim_corrupt(s, 0, &id) == EFS_OK, "corrupt");
        CHECK(efs_sim_frag_healthy(s, &id) == 0, "damaged fragment stays damaged");
        memset(got, 0x5a, sizeof(got));
        CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK &&
                  memcmp(src, got, sizeof(src)) == 0,
              "reconstructed");
        CHECK(efs_sim_frag_present(s, &id) == 1, "fragment not deleted");
        CHECK(efs_sim_frag_healthy(s, &id) == 0, "not repaired");
        efs_sim_free(s);
    }
    finish("one_fragment_corrupt",
           "each fragment position reconstructs; the damaged copy is not repaired");
}

static void case_missing_checksum(void)
{
    struct efs_sim *s = mk(120);
    efs_ino_t ino = 0;
    struct efs_frag_id id;
    uint8_t src[64], got[64];

    begin();
    CHECK(s, "mk");
    if (!s) {
        finish("missing_checksum", "simulator did not start");
        return;
    }
    fill(src, sizeof(src), 0x61);
    CHECK(publish_file(s, "sum", src, sizeof(src), &ino) == EFS_OK, "publish");
    CHECK(frag_of(s, ino, 0, &id) == EFS_OK, "id");
    CHECK(efs_sim_drop_sum(s, &id) == EFS_OK, "drop sum");
    CHECK(efs_sim_frag_healthy(s, &id) == 0, "missing sum is not trusted");
    memset(got, 0x5a, sizeof(got));
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK &&
              memcmp(src, got, sizeof(src)) == 0,
          "read uses the other fragments");
    CHECK(efs_sim_corrupt(s, 0, &id) == EFS_OK, "corrupt payload too");
    CHECK(efs_sim_frag_healthy(s, &id) == 0, "bad payload without a sum stays out");
    memset(got, 0x5a, sizeof(got));
    CHECK(efs_sim_read_chunk(s, ino, 0, got, sizeof(got)) == EFS_OK &&
              memcmp(src, got, sizeof(src)) == 0,
          "bad payload was not served");
    efs_sim_free(s);
    finish("missing_checksum",
           "sim refuses a fragment with no sidecar, including after its payload is flipped");
}

static void case_two_fragments(void)
{
    struct efs_sim *s = mk(130);
    efs_ino_t ino = 0;
    struct efs_frag_id a, b;
    uint8_t src[64], got[64];
    int rc;

    begin();
    CHECK(s, "mk");
    if (!s) {
        finish("two_fragments_unavailable", "simulator did not start");
        return;
    }
    fill(src, sizeof(src), 0x72);
    CHECK(publish_file(s, "two", src, sizeof(src), &ino) == EFS_OK, "publish");
    CHECK(frag_of(s, ino, 0, &a) == EFS_OK, "id0");
    CHECK(frag_of(s, ino, 1, &b) == EFS_OK, "id1");
    CHECK(efs_sim_corrupt(s, 0, &a) == EFS_OK, "corrupt 0");
    CHECK(efs_sim_corrupt(s, 0, &b) == EFS_OK, "corrupt 1");
    memset(got, 0xa5, sizeof(got));
    rc = efs_sim_read_chunk(s, ino, 0, got, sizeof(got));
    CHECK(rc != EFS_OK, "read fails");
    CHECK(got[0] == 0xa5 && got[sizeof(got) - 1] == 0xa5,
          "failed read did not write a substitute");
    efs_sim_free(s);
    finish("two_fragments_unavailable",
           "two damaged fragments are an explicit read failure");
}

static void case_repair_gap(void)
{
    emit("repair_restores_protection", "GAP",
         "no repair; a readable 2-of-3 chunk is not fully protected "
         "and a restarted node does not rebuild lost fragments");
    gaps++;
}

int main(void)
{
    case_one_server_lost();
    case_restart();
    case_one_corrupt();
    case_missing_checksum();
    case_two_fragments();
    case_repair_gap();
    if (failures)
        return 1;
    if (gaps)
        return 2;
    return 0;
}
