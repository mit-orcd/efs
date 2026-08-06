#ifndef EFS_ERASURE_H
#define EFS_ERASURE_H

#include "efs/common.h"

/* Encode a 128 KiB chunk into 3 fragments of 64 KiB each.
 * fragments[0] = D1 (first half)
 * fragments[1] = D2 (second half)
 * fragments[2] = P  = D1 XOR D2
 *
 * chunk_len may be <= EFS_CHUNK_SIZE; shorter input is zero-padded
 * internally.  Output buffers are always EFS_FRAGMENT_SIZE bytes.
 */
int efs_encode_chunk(const uint8_t *chunk, size_t chunk_len,
                     uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE]);

/* Reconstruct a 128 KiB chunk from any 2 of the 3 fragments.
 * missing is the index (0,1,2) of the fragment that is absent.
 * have_a and have_b are the indices of the two fragments present.
 * chunk_len is the original unpadded chunk length (<= EFS_CHUNK_SIZE).
 */
int efs_decode_chunk(const uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE],
                     int have_a, int have_b, int missing,
                     uint8_t *chunk, size_t chunk_len);

#endif
