#include "efs/erasure.h"
#include "efs/checksum.h"
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

    /* In-place encode: fragments[0]/[1] alias the source (write hot path). */
    {
        size_t cs = EFS_CHUNK_SIZE;
        size_t fl = cs / 2;
        uint8_t *chunk = malloc(cs);
        uint8_t *parity = malloc(fl);
        uint8_t *decoded = malloc(cs);
        uint8_t *sep = malloc(cs);
        if (!chunk || !parity || !decoded || !sep) {
            fprintf(stderr, "FAIL alias oom\n");
            failures++;
        } else {
            for (size_t i = 0; i < cs; i++)
                chunk[i] = (uint8_t)(i * 17 + 3);
            memcpy(sep, chunk, cs);
            uint8_t *frags[EFS_NUM_FRAGMENTS] = {chunk, chunk + fl, parity};
            if (efs_encode_chunk(chunk, cs, cs, frags) != 0) {
                fprintf(stderr, "FAIL alias encode\n");
                failures++;
            } else if (memcmp(chunk, sep, cs) != 0) {
                fprintf(stderr, "FAIL alias mutated source data halves\n");
                failures++;
            } else {
                uint8_t *dfrags[EFS_NUM_FRAGMENTS] = {frags[0], frags[1], frags[2]};
                if (efs_decode_chunk(dfrags, cs, 0, 1, 2, decoded, cs) != 0 ||
                    memcmp(decoded, sep, cs) != 0) {
                    fprintf(stderr, "FAIL alias decode\n");
                    failures++;
                }
            }
        }
        free(chunk);
        free(parity);
        free(decoded);
        free(sep);
    }

    /* W154: each fragment is the XOR of the other two, whichever is missing,
     * and the rebuilt bytes hash to the published checksum. */
    {
        size_t cs = EFS_CHUNK_SIZE, fl = cs / 2;
        uint8_t *chunk = malloc(cs), *buf = malloc(4 * fl);
        if (!chunk || !buf) {
            fprintf(stderr, "FAIL rebuild oom\n");
            failures++;
        } else {
            uint8_t *frags[EFS_NUM_FRAGMENTS] = {buf, buf + fl, buf + 2 * fl};
            uint8_t *out = buf + 3 * fl;
            for (size_t i = 0; i < cs; i++)
                chunk[i] = (uint8_t)(i * 131 + (i >> 9));
            if (efs_encode_chunk(chunk, cs - 777, cs, frags) != 0) {
                fprintf(stderr, "FAIL rebuild encode\n");
                failures++;
            }
            for (int m = 0; m < EFS_NUM_FRAGMENTS; m++) {
                uint8_t want[EFS_HASH_SIZE], got[EFS_HASH_SIZE];
                efs_hash(frags[m], fl, want);
                memset(out, 0xEE, fl);
                if (efs_rebuild_fragment(frags[(m + 1) % 3], frags[(m + 2) % 3], out, fl) != 0 ||
                    memcmp(out, frags[m], fl) != 0) {
                    fprintf(stderr, "FAIL rebuild fragment %d\n", m);
                    failures++;
                }
                efs_hash(out, fl, got);
                if (memcmp(got, want, sizeof(got)) != 0) {
                    fprintf(stderr, "FAIL rebuild checksum %d\n", m);
                    failures++;
                }
            }
            if (efs_rebuild_fragment(frags[0], frags[1], frags[0], fl) != EFS_ERR_INVAL ||
                efs_rebuild_fragment(frags[0], frags[1], out, 0) != EFS_ERR_INVAL ||
                efs_rebuild_fragment(NULL, frags[1], out, fl) != EFS_ERR_INVAL) {
                fprintf(stderr, "FAIL rebuild argument checks\n");
                failures++;
            }
        }
        free(chunk);
        free(buf);
    }

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
