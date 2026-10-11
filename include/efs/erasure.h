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

/* W95 staged segment codec: each fragment is a multiple of 4 KiB. A segment
 * is a separately versioned object, not an ordinary chunk. No current client
 * PUT or metadata path selects this codec; publication/GC integration is owed.
 * Geometry includes the packed body's headers/index but excludes store tails.
 * Decode writes size bytes, so the output buffer must hold the full size.
 * All three fragment buffers must be allocated, including the missing one.
 */
#define EFS_SEGMENT_ALIGNMENT (8u * 1024u)
#define EFS_SEGMENT_MAX_SIZE EFS_MIN_CHUNK_SIZE
int efs_segment_geometry(size_t body_bytes, size_t *size);
int efs_encode_segment(const uint8_t *body, size_t len, size_t size,
                       uint8_t *fragments[EFS_NUM_FRAGMENTS]);
int efs_decode_segment(uint8_t *const fragments[EFS_NUM_FRAGMENTS], size_t size,
                       int have_a, int have_b, int missing, uint8_t *body, size_t len);

#endif
