#include "efs/common.h"
#include "efs/local_ec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fail;

static void expect_ok(const char *name, int rc)
{
    if (rc != EFS_OK) {
        fprintf(stderr, "FAIL %s: rc=%d\n", name, rc);
        fail++;
    }
}

static void test_roundtrip(uint32_t n)
{
    uint8_t frag[EFS_FRAGMENT_SIZE];
    for (uint32_t i = 0; i < EFS_FRAGMENT_SIZE; i++)
        frag[i] = (uint8_t)(i * 17u + n);

    uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD];
    expect_ok("encode", efs_local_ec_encode(n, frag, EFS_FRAGMENT_SIZE, shards));

    int have[EFS_MAX_STORAGE_PATHS];
    for (uint32_t i = 0; i < n; i++)
        have[i] = 1;

    uint8_t out[EFS_FRAGMENT_SIZE];
    expect_ok("decode-all",
              efs_local_ec_decode(n, shards, have, out, EFS_FRAGMENT_SIZE));
    if (memcmp(frag, out, EFS_FRAGMENT_SIZE) != 0) {
        fprintf(stderr, "FAIL n=%u roundtrip mismatch\n", n);
        fail++;
    }
}

static void test_drop(uint32_t n, const int drop[], int ndrop)
{
    uint8_t frag[EFS_FRAGMENT_SIZE];
    for (uint32_t i = 0; i < EFS_FRAGMENT_SIZE; i++)
        frag[i] = (uint8_t)(i ^ (n * 3));

    uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD];
    expect_ok("encode-drop", efs_local_ec_encode(n, frag, EFS_FRAGMENT_SIZE, shards));

    int have[EFS_MAX_STORAGE_PATHS];
    for (uint32_t i = 0; i < n; i++)
        have[i] = 1;
    for (int d = 0; d < ndrop; d++)
        have[drop[d]] = 0;

    uint8_t out[EFS_FRAGMENT_SIZE];
    int rc = efs_local_ec_decode(n, shards, have, out, EFS_FRAGMENT_SIZE);
    if (rc != EFS_OK) {
        fprintf(stderr, "FAIL n=%u drop decode rc=%d\n", n, rc);
        fail++;
        return;
    }
    if (memcmp(frag, out, EFS_FRAGMENT_SIZE) != 0) {
        fprintf(stderr, "FAIL n=%u drop data mismatch\n", n);
        fail++;
    }
}

int main(void)
{
    fail = 0;
    setlinebuf(stdout);
    setlinebuf(stderr);

    if (efs_local_ec_k(2) != 0 || efs_local_ec_m(2) != 0)
        fprintf(stderr, "note: n=2 unsupported as expected\n");

    test_roundtrip(1);
    test_roundtrip(3);
    test_roundtrip(4);
    test_roundtrip(5);
    test_roundtrip(8);

    {
        int d[] = {0};
        test_drop(3, d, 1);
        d[0] = 1;
        test_drop(3, d, 1);
        d[0] = 2;
        test_drop(3, d, 1);
    }
    {
        int d[] = {0, 1};
        test_drop(4, d, 2);
        d[0] = 1;
        d[1] = 2;
        test_drop(4, d, 2);
        d[0] = 0;
        d[1] = 3;
        test_drop(4, d, 2);
    }
    {
        int d[] = {0, 1};
        test_drop(5, d, 2);
        d[0] = 2;
        d[1] = 4;
        test_drop(5, d, 2);
    }

    /* Too many losses must fail. */
    {
        uint8_t frag[EFS_FRAGMENT_SIZE];
        memset(frag, 0xab, sizeof(frag));
        uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD];
        efs_local_ec_encode(3, frag, EFS_FRAGMENT_SIZE, shards);
        int have[EFS_MAX_STORAGE_PATHS] = {0, 0, 0};
        uint8_t out[EFS_FRAGMENT_SIZE];
        if (efs_local_ec_decode(3, shards, have, out, EFS_FRAGMENT_SIZE) == EFS_OK) {
            fprintf(stderr, "FAIL expected decode failure with 0 shards\n");
            fail++;
        }
    }

    if (fail == 0)
        printf("test_local_ec: OK\n");
    else
        printf("test_local_ec: %d failures\n", fail);
    return fail ? 1 : 0;
}
