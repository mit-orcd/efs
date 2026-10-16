#ifndef EFS_ERASURE_H
#define EFS_ERASURE_H

#include "efs/common.h"

/* Encode a data chunk into 3 fragments of frag_len = chunk_size/2 each.
 * fragments[0] = D1 (first half)
 * fragments[1] = D2 (second half)
 * fragments[2] = P  = D1 XOR D2
 *
 * chunk_len may be <= chunk_size; shorter input is zero-padded.
 * Each fragments[i] must point to at least frag_len bytes.
 */
int efs_encode_chunk(const uint8_t *chunk, size_t chunk_len, size_t chunk_size,
                     uint8_t *fragments[EFS_NUM_FRAGMENTS]);

/* Reconstruct a chunk from any 2 of the 3 fragments.
 * missing is the index (0,1,2) of the fragment that is absent.
 * have_a and have_b are the indices of the two fragments present.
 */
int efs_decode_chunk(uint8_t *const fragments[EFS_NUM_FRAGMENTS], size_t chunk_size,
                     int have_a, int have_b, int missing,
                     uint8_t *chunk, size_t chunk_len);

#endif
