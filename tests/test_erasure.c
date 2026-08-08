#include "efs/erasure.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int failures = 0;

static void test_roundtrip(const char *label, size_t chunk_size, size_t len)
{
    uint8_t *chunk = malloc(chunk_size);
    uint8_t *decoded = malloc(chunk_size);
    size_t frag_len = chunk_size / 2;
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
    if (!chunk || !decoded || !frag_buf) {
        fprintf(stderr, "FAIL %s oom\n", label);
        failures++;
        free(chunk);
        free(decoded);
        free(frag_buf);
        return;
    }
    uint8_t *fragments[EFS_NUM_FRAGMENTS] = {
        frag_buf, frag_buf + frag_len, frag_buf + 2 * frag_len
    };

    for (size_t i = 0; i < len; i++)
        chunk[i] = (uint8_t)(rand() & 0xFF);
    if (len < chunk_size)
        memset(chunk + len, 0, chunk_size - len);

    int rc = efs_encode_chunk(chunk, len, chunk_size, fragments);
    if (rc != 0) {
        fprintf(stderr, "FAIL %s encode: %d\n", label, rc);
        failures++;
        goto out;
    }

    for (int missing = 0; missing < EFS_NUM_FRAGMENTS; missing++) {
        int a = (missing + 1) % EFS_NUM_FRAGMENTS;
        int b = (missing + 2) % EFS_NUM_FRAGMENTS;
        memset(decoded, 0, chunk_size);
        rc = efs_decode_chunk(fragments, chunk_size, a, b, missing, decoded, len);
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

out:
    free(chunk);
    free(decoded);
    free(frag_buf);
}

int main(void)
{
    srand((unsigned)time(NULL));

    test_roundtrip("empty", EFS_CHUNK_SIZE, 0);
    test_roundtrip("1", EFS_CHUNK_SIZE, 1);
    test_roundtrip("partial", EFS_CHUNK_SIZE, EFS_FRAGMENT_SIZE - 1);
    test_roundtrip("exact-half", EFS_CHUNK_SIZE, EFS_FRAGMENT_SIZE);
    test_roundtrip("full", EFS_CHUNK_SIZE, EFS_CHUNK_SIZE);

    const size_t meg = 1024 * 1024;
    test_roundtrip("1M-empty", meg, 0);
    test_roundtrip("1M-partial", meg, meg / 2 - 1);
    test_roundtrip("1M-full", meg, meg);

    if (!efs_chunk_size_valid(EFS_DEFAULT_CHUNK_SIZE) ||
        !efs_chunk_size_valid(EFS_MAX_CHUNK_SIZE) ||
        efs_chunk_size_valid(EFS_MIN_CHUNK_SIZE - 1) ||
        efs_chunk_size_valid(3 * 1024 * 1024)) {
        fprintf(stderr, "FAIL efs_chunk_size_valid\n");
        failures++;
    }

    if (failures == 0) {
        printf("test_erasure: OK\n");
        return 0;
    }
    printf("test_erasure: %d failures\n", failures);
    return 1;
}
