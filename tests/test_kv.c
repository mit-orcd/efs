/* Isolated ordered-KV tests. No sockets, no cluster. */
#include "efs/kv.h"
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

int main(void)
{
    test_mem_kv();
    if (failures) {
        fprintf(stderr, "test_kv: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv: OK\n");
    return 0;
}
