/* Isolated ordered-KV tests. No sockets, no cluster. */
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

struct scan_acc {
    char keys[8][16];
    int n;
};

static int acc_cb(void *user, const uint8_t *key, uint32_t klen,
                  const uint8_t *val, uint32_t vlen)
{
    struct scan_acc *a = user;
    (void)val;
    (void)vlen;
    if (a->n >= 8)
        return 1;
    if (klen >= 16)
        return EFS_ERR_INVAL;
    memcpy(a->keys[a->n], key, klen);
    a->keys[a->n][klen] = 0;
    a->n++;
    return 0;
}

static void test_mem_kv(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t buf[32];
    uint32_t len;
    struct scan_acc acc = { 0 };
    const uint8_t *k_b = (const uint8_t *)"b";
    const uint8_t *k_a = (const uint8_t *)"a";
    const uint8_t *k_c = (const uint8_t *)"c";
    const uint8_t *k_aa = (const uint8_t *)"aa";

    CHECK(kv != NULL, "create");
    CHECK(efs_kv_put(kv, k_b, 1, (const uint8_t *)"vb", 2) == EFS_OK, "put b");
    CHECK(efs_kv_put(kv, k_c, 1, (const uint8_t *)"vc", 2) == EFS_OK, "put c");
    CHECK(efs_kv_put(kv, k_a, 1, (const uint8_t *)"va", 2) == EFS_OK, "put a");
    CHECK(efs_kv_put(kv, k_aa, 2, (const uint8_t *)"vaa", 3) == EFS_OK, "put aa");

    len = sizeof(buf);
    CHECK(efs_kv_get(kv, k_a, 1, buf, &len) == EFS_OK && len == 2 &&
              memcmp(buf, "va", 2) == 0,
          "get a");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, (const uint8_t *)"z", 1, buf, &len) == EFS_ERR_NOT_FOUND,
          "miss");

    /* overwrite */
    CHECK(efs_kv_put(kv, k_a, 1, (const uint8_t *)"VA", 2) == EFS_OK, "overwrite");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, k_a, 1, buf, &len) == EFS_OK && memcmp(buf, "VA", 2) == 0,
          "get overwrite");

    CHECK(efs_kv_scan(kv, acc_cb, &acc) == EFS_OK, "scan");
    CHECK(acc.n == 4, "scan n");
    CHECK(strcmp(acc.keys[0], "a") == 0 && strcmp(acc.keys[1], "aa") == 0 &&
              strcmp(acc.keys[2], "b") == 0 && strcmp(acc.keys[3], "c") == 0,
          "scan order");

    CHECK(efs_kv_del(kv, k_b, 1) == EFS_OK, "del b");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, k_b, 1, buf, &len) == EFS_ERR_NOT_FOUND, "gone");
    CHECK(efs_kv_del(kv, k_b, 1) == EFS_ERR_NOT_FOUND, "del miss");

    efs_kv_mem_free(kv);
}

static void test_batch_and_prefix(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    uint8_t buf[32];
    uint32_t len;
    struct scan_acc acc = { 0 };
    struct efs_kv_item it[3];
    const uint8_t *k_a = (const uint8_t *)"a";
    const uint8_t *k_aa = (const uint8_t *)"aa";
    const uint8_t *k_b = (const uint8_t *)"b";
    const uint8_t *k_z = (const uint8_t *)"z";

    CHECK(kv != NULL, "create");
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_PUT;
    it[0].key = k_a;
    it[0].klen = 1;
    it[0].val = (const uint8_t *)"1";
    it[0].vlen = 1;
    it[1].op = EFS_KV_PUT;
    it[1].key = k_aa;
    it[1].klen = 2;
    it[1].val = (const uint8_t *)"2";
    it[1].vlen = 1;
    it[2].op = EFS_KV_PUT;
    it[2].key = k_b;
    it[2].klen = 1;
    it[2].val = (const uint8_t *)"3";
    it[2].vlen = 1;
    CHECK(efs_kv_batch(kv, it, 3) == EFS_OK, "batch put");

    len = 0;
    CHECK(efs_kv_get(kv, k_a, 1, NULL, &len) == EFS_ERR_INVAL && len == 1,
          "size probe");

    CHECK(efs_kv_scan_prefix(kv, k_a, 1, acc_cb, &acc) == EFS_OK, "prefix");
    CHECK(acc.n == 2, "prefix n");
    CHECK(strcmp(acc.keys[0], "a") == 0 && strcmp(acc.keys[1], "aa") == 0,
          "prefix order");

    CHECK(efs_kv_mem_fail_next_batch(kv) == EFS_OK, "arm fail");
    it[0].key = k_z;
    it[0].val = (const uint8_t *)"9";
    CHECK(efs_kv_batch(kv, it, 1) == EFS_ERR_IO, "fail next");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, k_z, 1, buf, &len) == EFS_ERR_NOT_FOUND, "atomic");

    it[0].op = EFS_KV_DEL;
    it[0].key = k_a;
    it[0].klen = 1;
    it[0].val = NULL;
    it[0].vlen = 0;
    it[1].op = EFS_KV_DEL;
    it[1].key = k_aa;
    it[1].klen = 2;
    CHECK(efs_kv_batch(kv, it, 2) == EFS_OK, "batch del");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, k_a, 1, buf, &len) == EFS_ERR_NOT_FOUND, "a gone");
    len = sizeof(buf);
    CHECK(efs_kv_get(kv, k_b, 1, buf, &len) == EFS_OK, "b kept");

    efs_kv_mem_free(kv);
}

static void test_dir_lanes(void)
{
    uint32_t seen[64];
    uint8_t lane;
    efs_ino_t ino = 1;
    int i, n = 0;

    memset(seen, 0xff, sizeof(seen));
    for (lane = 0; lane < 64; lane++) {
        uint32_t sh = efs_kv_lane_shard(ino, lane);
        for (i = 0; i < n; i++) {
            if (seen[i] == sh)
                break;
        }
        CHECK(i == n, "64 distinct dir/file lanes");
        if (i == n && n < 64)
            seen[n++] = sh;
    }
    CHECK(efs_kv_dentry_shard(ino, "x", 0) == efs_kv_inode_shard(ino), "LOCAL");
    CHECK(efs_kv_dentry_shard(ino, "x", 2) ==
              efs_kv_lane_shard(ino, efs_kv_dir_lane("x")),
          "HASHED");
}

int main(void)
{
    test_mem_kv();
    test_batch_and_prefix();
    test_dir_lanes();
    if (failures) {
        fprintf(stderr, "test_kv: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv: OK\n");
    return 0;
}
