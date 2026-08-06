#include "efs/erasure.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int failures = 0;

static void test_roundtrip(const char *label, size_t len)
{
    uint8_t *chunk = malloc(EFS_CHUNK_SIZE);
    uint8_t decoded[EFS_CHUNK_SIZE];
    uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE];

    for (size_t i = 0; i < len; i++)
        chunk[i] = (uint8_t)(rand() & 0xFF);

    memset(chunk + len, 0, EFS_CHUNK_SIZE - len);

    int rc = efs_encode_chunk(chunk, len, fragments);
    if (rc != 0) {
        fprintf(stderr, "FAIL %s encode: %d\n", label, rc);
        failures++;
        free(chunk);
        return;
    }

    for (int missing = 0; missing < EFS_NUM_FRAGMENTS; missing++) {
        int a = (missing + 1) % EFS_NUM_FRAGMENTS;
        int b = (missing + 2) % EFS_NUM_FRAGMENTS;
        memset(decoded, 0, sizeof(decoded));
        rc = efs_decode_chunk(fragments, a, b, missing, decoded, len);
        if (rc != 0) {
            fprintf(stderr, "FAIL %s decode missing=%d: %d\n", label, missing, rc);
            failures++;
            continue;
        }
        if (memcmp(chunk, decoded, len) != 0) {
            fprintf(stderr, "FAIL %s decode missing=%d data mismatch\n", label, missing);
            failures++;
        }
    }

    free(chunk);
}

int main(void)
{
    srand((unsigned)time(NULL));

    test_roundtrip("empty", 0);
    test_roundtrip("1", 1);
    test_roundtrip("partial", EFS_FRAGMENT_SIZE - 1);
    test_roundtrip("exact-half", EFS_FRAGMENT_SIZE);
    test_roundtrip("full", EFS_CHUNK_SIZE);

    if (failures == 0) {
        printf("test_erasure: OK\n");
        return 0;
    }
    printf("test_erasure: %d failures\n", failures);
    return 1;
}
