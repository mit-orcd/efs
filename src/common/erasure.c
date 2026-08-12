#include "efs/erasure.h"
#include <stdint.h>
#include <string.h>

/* Byte-wise XOR is the hot loop of 2+1 parity; do it word-at-a-time (and let
 * the compiler vectorize) to cut per-chunk CPU on both encode and decode. */
static void xor_into(uint8_t *dst, const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i = 0;
    for (; i + sizeof(uint64_t) <= n; i += sizeof(uint64_t)) {
        uint64_t va, vb;
        memcpy(&va, a + i, sizeof(va));
        memcpy(&vb, b + i, sizeof(vb));
        uint64_t r = va ^ vb;
        memcpy(dst + i, &r, sizeof(r));
    }
    for (; i < n; i++)
        dst[i] = a[i] ^ b[i];
}
#include <string.h>
#include <stdio.h>

int efs_encode_chunk(const uint8_t *chunk, size_t chunk_len, size_t chunk_size,
                     uint8_t *fragments[EFS_NUM_FRAGMENTS])
{
    if (!chunk || !fragments || !fragments[0] || !fragments[1] || !fragments[2])
        return EFS_ERR_INVAL;
    if (chunk_size < EFS_MIN_CHUNK_SIZE || chunk_size > EFS_MAX_CHUNK_SIZE)
        return EFS_ERR_INVAL;
    if ((chunk_size & 1u) != 0)
        return EFS_ERR_INVAL;
    if (chunk_len > chunk_size)
        return EFS_ERR_INVAL;

    size_t frag_len = chunk_size / 2;

    if (chunk_len >= frag_len) {
        memcpy(fragments[0], chunk, frag_len);
        size_t second = chunk_len - frag_len;
        if (second > frag_len)
            second = frag_len;
        if (second > 0)
            memcpy(fragments[1], chunk + frag_len, second);
        if (second < frag_len)
            memset(fragments[1] + second, 0, frag_len - second);
    } else {
        if (chunk_len > 0)
            memcpy(fragments[0], chunk, chunk_len);
        memset(fragments[0] + chunk_len, 0, frag_len - chunk_len);
        memset(fragments[1], 0, frag_len);
    }

    xor_into(fragments[2], fragments[0], fragments[1], frag_len);

    return EFS_OK;
}

int efs_decode_chunk(uint8_t *const fragments[EFS_NUM_FRAGMENTS], size_t chunk_size,
                     int have_a, int have_b, int missing,
                     uint8_t *chunk, size_t chunk_len)
{
    if (!fragments || !fragments[0] || !fragments[1] || !fragments[2] || !chunk)
        return EFS_ERR_INVAL;
    if (chunk_size < EFS_MIN_CHUNK_SIZE || chunk_size > EFS_MAX_CHUNK_SIZE)
        return EFS_ERR_INVAL;
    if ((chunk_size & 1u) != 0)
        return EFS_ERR_INVAL;
    if (chunk_len > chunk_size)
        return EFS_ERR_INVAL;
    if (have_a < 0 || have_a >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (have_b < 0 || have_b >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (missing < 0 || missing >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (have_a == have_b || have_a == missing || have_b == missing)
        return EFS_ERR_INVAL;

    size_t frag_len = chunk_size / 2;

    if (missing == 2) {
        memcpy(chunk, fragments[0], frag_len);
        memcpy(chunk + frag_len, fragments[1], frag_len);
    } else if (missing == 1) {
        memcpy(chunk, fragments[0], frag_len);
        xor_into(chunk + frag_len, fragments[0], fragments[2], frag_len);
    } else { /* missing == 0 */
        xor_into(chunk, fragments[1], fragments[2], frag_len);
        memcpy(chunk + frag_len, fragments[1], frag_len);
    }

    (void)chunk_len;
    return EFS_OK;
}
