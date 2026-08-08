#ifndef EFS_CHECKSUM_H
#define EFS_CHECKSUM_H

#include "efs/common.h"

/* Compute the Blake3 hash of len bytes from data into out (32 bytes). */
void efs_hash(const void *data, size_t len, uint8_t out[EFS_HASH_SIZE]);

/* Blake3 of an all-zero buffer of frag_len bytes (cached for common sizes). */
void efs_hash_zero_fragment_len(size_t frag_len, uint8_t out[EFS_HASH_SIZE]);

/* Blake3 of an all-zero EFS_FRAGMENT_SIZE (default) buffer. */
void efs_hash_zero_fragment(uint8_t out[EFS_HASH_SIZE]);

/* True if len bytes at data are all zero. */
int efs_bytes_are_zero(const void *data, size_t len);

/* Pointer to at least `len` zero bytes (len ≤ EFS_MAX_FRAGMENT_SIZE).
 * Valid for the process lifetime; do not free. */
const uint8_t *efs_zero_bytes(size_t len);

/* Format a 32-byte hash as 64 hex characters (no null terminator). */
void efs_hash_to_hex(const uint8_t hash[EFS_HASH_SIZE], char hex[EFS_HASH_SIZE * 2 + 1]);

/* Parse 64 hex characters into a 32-byte hash. Returns 0 on success. */
int efs_hash_from_hex(const char hex[EFS_HASH_SIZE * 2 + 1], uint8_t hash[EFS_HASH_SIZE]);

#endif
