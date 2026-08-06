#include "efs/erasure.h"
#include <string.h>
#include <stdio.h>

int efs_encode_chunk(const uint8_t *chunk, size_t chunk_len,
                     uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE])
{
    if (chunk_len > EFS_CHUNK_SIZE)
        return EFS_ERR_INVAL;

    /* Avoid zeroing all three fragment buffers (192 KiB) up front — that
     * dominated client profiles on the medium write load. Copy each data
     * half and pad only the unused tail; parity is fully written by XOR. */
    if (chunk_len >= EFS_FRAGMENT_SIZE) {
        memcpy(fragments[0], chunk, EFS_FRAGMENT_SIZE);
        size_t second = chunk_len - EFS_FRAGMENT_SIZE;
        if (second > EFS_FRAGMENT_SIZE)
            second = EFS_FRAGMENT_SIZE;
        if (second > 0)
            memcpy(fragments[1], chunk + EFS_FRAGMENT_SIZE, second);
        if (second < EFS_FRAGMENT_SIZE)
            memset(fragments[1] + second, 0, EFS_FRAGMENT_SIZE - second);
    } else {
        if (chunk_len > 0)
            memcpy(fragments[0], chunk, chunk_len);
        memset(fragments[0] + chunk_len, 0, EFS_FRAGMENT_SIZE - chunk_len);
        memset(fragments[1], 0, EFS_FRAGMENT_SIZE);
    }

    for (size_t i = 0; i < EFS_FRAGMENT_SIZE; i++)
        fragments[2][i] = fragments[0][i] ^ fragments[1][i];

    return EFS_OK;
}

int efs_decode_chunk(const uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE],
                     int have_a, int have_b, int missing,
                     uint8_t *chunk, size_t chunk_len)
{
    if (chunk_len > EFS_CHUNK_SIZE)
        return EFS_ERR_INVAL;
    if (have_a < 0 || have_a >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (have_b < 0 || have_b >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (missing < 0 || missing >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (have_a == have_b || have_a == missing || have_b == missing)
        return EFS_ERR_INVAL;

    if (missing == 2) {
        memcpy(chunk, fragments[0], EFS_FRAGMENT_SIZE);
        memcpy(chunk + EFS_FRAGMENT_SIZE, fragments[1], EFS_FRAGMENT_SIZE);
    } else if (missing == 1) {
        memcpy(chunk, fragments[0], EFS_FRAGMENT_SIZE);
        for (size_t i = 0; i < EFS_FRAGMENT_SIZE; i++)
            chunk[EFS_FRAGMENT_SIZE + i] = fragments[0][i] ^ fragments[2][i];
    } else { /* missing == 0 */
        for (size_t i = 0; i < EFS_FRAGMENT_SIZE; i++)
            chunk[i] = fragments[1][i] ^ fragments[2][i];
        memcpy(chunk + EFS_FRAGMENT_SIZE, fragments[1], EFS_FRAGMENT_SIZE);
    }

    (void)chunk_len; /* padding zeros are intentionally ignored */
    return EFS_OK;
}
