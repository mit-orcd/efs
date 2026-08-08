#include "efs/erasure.h"
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

    for (size_t i = 0; i < frag_len; i++)
        fragments[2][i] = fragments[0][i] ^ fragments[1][i];

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
        for (size_t i = 0; i < frag_len; i++)
            chunk[frag_len + i] = fragments[0][i] ^ fragments[2][i];
    } else { /* missing == 0 */
        for (size_t i = 0; i < frag_len; i++)
            chunk[i] = fragments[1][i] ^ fragments[2][i];
        memcpy(chunk + frag_len, fragments[1], frag_len);
    }

    (void)chunk_len;
    return EFS_OK;
}
