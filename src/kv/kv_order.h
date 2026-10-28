#ifndef EFS_KV_ORDER_H
#define EFS_KV_ORDER_H
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <string.h>
/* Same unsigned byte ordering in memory, segments and snapshot merge. */
static inline int kv_key_cmp(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl)
{
    uint32_t n=al<bl ? al : bl;
    int c=n ? memcmp(a,b,n) : 0;
    return c ? c : al<bl ? -1 : al>bl ? 1 : 0;
}
/* Exclude node-local and malformed prefixes from BOTH export paths. */
static inline int kv_key_in_group(const uint8_t *key, uint32_t klen, uint8_t group)
{
    if (klen<2) return 0;
    uint32_t shard=(uint32_t)key[0]<<8 | key[1];
    return shard<=EFS_KV_SHARD_MASK && efs_raft_shard_group(shard)==group;
}
#endif
