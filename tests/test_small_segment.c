#include "efs/erasure.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    size_t size = 123;
    assert(efs_segment_geometry(0, &size) == EFS_ERR_INVAL && size == 123);
    assert(efs_segment_geometry(SIZE_MAX, &size) == EFS_ERR_INVAL);
    assert(efs_segment_geometry(1, NULL) == EFS_ERR_INVAL);
    assert(efs_segment_geometry(EFS_SEGMENT_MAX_SIZE + 1, &size) == EFS_ERR_INVAL);
    for (size_t want = 1; want <= EFS_SEGMENT_MAX_SIZE; want++) {
        assert(!efs_segment_geometry(want, &size));
        assert(size >= want && size - want < EFS_SEGMENT_ALIGNMENT);
        assert(size % EFS_SEGMENT_ALIGNMENT == 0);
    }
    uint8_t *source = malloc(EFS_SEGMENT_MAX_SIZE);
    uint8_t *out = malloc(EFS_SEGMENT_MAX_SIZE + 16);
    uint8_t *all = malloc(EFS_SEGMENT_MAX_SIZE * 3 / 2);
    assert(source && out && all);
    for (size_t i = 0; i < EFS_SEGMENT_MAX_SIZE; i++) source[i] = (uint8_t)(i * 37 + i / 17);
    for (size = EFS_SEGMENT_ALIGNMENT; size <= EFS_SEGMENT_MAX_SIZE; size += EFS_SEGMENT_ALIGNMENT) {
        size_t half = size / 2;
        uint8_t *f[3] = {all, all + half, all + 2 * half};
        size_t lengths[] = {0, 1, 17, 512, half - 1, half, half + 1, size};
        for (unsigned k = 0; k < sizeof(lengths)/sizeof(lengths[0]); k++) {
            size_t len = lengths[k];
            assert(!efs_encode_segment(source, len, size, f));
            for (size_t i = 0; i < half; i++) assert(f[2][i] == (uint8_t)(f[0][i] ^ f[1][i]));
            for (int missing = 0; missing < 3; missing++) {
                memset(f[missing], 0xa5, half);
                memset(out, 0xbb, size + 16);
                assert(!efs_decode_segment(f, size, (missing+1)%3, (missing+2)%3, missing, out, len));
                assert(!memcmp(out, source, len));
                for (size_t i = len; i < size; i++) assert(out[i] == 0);
                for (size_t i = size; i < size + 16; i++) assert(out[i] == 0xbb);
                assert(!efs_encode_segment(source, len, size, f));
            }
            /* Existing ordinary chunks must still reject small geometries. */
            if (size < EFS_MIN_CHUNK_SIZE) {
                assert(efs_encode_chunk(source, len, size, f) == EFS_ERR_INVAL);
                assert(efs_decode_chunk(f, size, 0, 1, 2, out, len) == EFS_ERR_INVAL);
            }
        }
    }
    uint8_t *f[3] = {all, all + 65536, all + 131072};
    size_t bad[] = {0, 2, 4096, 8193, 12288, EFS_SEGMENT_MAX_SIZE + 8192};
    for (unsigned k = 0; k < sizeof(bad)/sizeof(bad[0]); k++) {
        assert(efs_encode_segment(source, 1, bad[k], f) == EFS_ERR_INVAL);
        assert(efs_decode_segment(f, bad[k], 0, 1, 2, out, 1) == EFS_ERR_INVAL);
    }
    assert(efs_encode_segment(source, 8193, 8192, f) == EFS_ERR_INVAL);
    assert(efs_decode_segment(f, 8192, 0, 0, 2, out, 1) == EFS_ERR_INVAL);
    assert(efs_decode_segment(f, 8192, 0, 2, 2, out, 1) == EFS_ERR_INVAL);
    assert(efs_decode_segment(f, 8192, -1, 1, 2, out, 1) == EFS_ERR_INVAL);
    /* Exact old/new codec equivalence at the shared 128 KiB geometry. */
    uint8_t *old = malloc(EFS_SEGMENT_MAX_SIZE * 3 / 2); assert(old);
    uint8_t *g[3] = {old, old + 65536, old + 131072};
    assert(!efs_encode_chunk(source, 512, EFS_MIN_CHUNK_SIZE, g));
    assert(!efs_encode_segment(source, 512, EFS_MIN_CHUNK_SIZE, f));
    assert(!memcmp(old, all, EFS_SEGMENT_MAX_SIZE * 3 / 2));
    free(old); free(source); free(out); free(all);
    puts("small segment geometry: all 2-of-3 recovery, padding, bounds and legacy isolation PASS");
    return 0;
}
