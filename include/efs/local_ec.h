#ifndef EFS_LOCAL_EC_H
#define EFS_LOCAL_EC_H

#include "efs/common.h"

/* Max bytes per local shard (4096-aligned, covers worst-case pad). */
#define EFS_LOCAL_EC_MAX_SHARD 65536

/* Number of data shards (k) for n local paths. n==1 → 1; n==3 → 2; n>=4 → n-2. */
uint32_t efs_local_ec_k(uint32_t n);

/* Parity count m: 0 (n==1), 1 (n==3), 2 (n>=4). */
uint32_t efs_local_ec_m(uint32_t n);

/* Shard payload length (4096-aligned) for each of the n shards. */
uint32_t efs_local_ec_shard_len(uint32_t n);

/* Encode a network fragment into n local shards.
 * shards[i] must have room for efs_local_ec_shard_len(n) bytes. */
int efs_local_ec_encode(uint32_t n, const uint8_t *frag, uint32_t frag_len,
                        uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD]);

/* Reconstruct frag from any available shards.
 * have[i] != 0 means shards[i] is valid. Needs at least k good shards. */
int efs_local_ec_decode(uint32_t n,
                        const uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD],
                        const int have[EFS_MAX_STORAGE_PATHS],
                        uint8_t *frag_out, uint32_t frag_len);

#endif
