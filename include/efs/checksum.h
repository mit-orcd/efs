#ifndef EFS_CHECKSUM_H
#define EFS_CHECKSUM_H

#include "efs/common.h"

/* Compute the Blake3 hash of len bytes from data into out (32 bytes). */
void efs_hash(const void *data, size_t len, uint8_t out[EFS_HASH_SIZE]);

/* CRC-32 (IEEE, reflected). For log-record framing, where the job is to
 * recognise a torn or rotted record, not to resist forgery — use efs_hash
 * for content identity. */
uint32_t efs_crc32(const void *data, size_t len);

/* Blake3 of an all-zero buffer of frag_len bytes (cached for common sizes). */
void efs_hash_zero_fragment_len(size_t frag_len, uint8_t out[EFS_HASH_SIZE]);

/* Blake3 of an all-zero EFS_FRAGMENT_SIZE (default) buffer. */
void efs_hash_zero_fragment(uint8_t out[EFS_HASH_SIZE]);

/* True if len bytes at data are all zero. */
int efs_bytes_are_zero(const void *data, size_t len);

#endif
