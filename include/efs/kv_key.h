#ifndef EFS_KV_KEY_H
#define EFS_KV_KEY_H

#include "efs/common.h"
#include "efs/opid.h"

/* Byte encoding of architecture.md §5 keys. Shard prefix multiplexes
 * isolated namespaces into one engine. Big-endian so range scans match
 * integer order. Kind bytes keep tuple types from colliding. */

#define EFS_KV_SHARD_BITS 12
#define EFS_KV_SHARD_MASK 0xFFFu
#define EFS_KV_KEY_MAX    320

#define EFS_KV_KIND_ALLOC  1
#define EFS_KV_KIND_INODE  2
#define EFS_KV_KIND_DENTRY 3
#define EFS_KV_KIND_CHUNK  4
#define EFS_KV_KIND_LANE   5
#define EFS_KV_KIND_OPID   6

static inline uint32_t efs_kv_inode_shard(efs_ino_t ino)
{
    return (uint32_t)(ino & EFS_KV_SHARD_MASK);
}

uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN]);
/* lane = ci % 64; lane 0 is the inode shard (architecture.md §7.3). */
uint32_t efs_kv_lane_shard(efs_ino_t ino, uint8_t lane);

int efs_kv_key_alloc(uint32_t shard, uint8_t *out, uint32_t *len);
int efs_kv_key_inode(uint32_t shard, efs_ino_t ino, uint8_t *out, uint32_t *len);
int efs_kv_key_dentry(uint32_t shard, efs_ino_t parent, const char *name,
                      uint8_t *out, uint32_t *len);
int efs_kv_key_dentry_prefix(uint32_t shard, efs_ino_t parent,
                             uint8_t *out, uint32_t *len);
int efs_kv_key_chunk(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                     uint32_t chunk_index, uint8_t *out, uint32_t *len);
int efs_kv_key_lane(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                    uint8_t *out, uint32_t *len);
int efs_kv_key_opid(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    uint32_t epoch, uint8_t *out, uint32_t *len);

#endif
