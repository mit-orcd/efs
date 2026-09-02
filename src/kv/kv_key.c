#include "efs/kv_key.h"
#include <string.h>

static void be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void be64(uint8_t *p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

static int start(uint8_t *out, uint32_t *len, uint32_t shard, uint8_t kind,
                 uint32_t extra, uint32_t cap)
{
    uint32_t n = 2 + 1 + extra;

    if (!out || !len)
        return EFS_ERR_INVAL;
    if (shard > EFS_KV_SHARD_MASK || n > cap || n > EFS_KV_KEY_MAX)
        return EFS_ERR_INVAL;
    be16(out, (uint16_t)shard);
    out[2] = kind;
    *len = n;
    return EFS_OK;
}

uint32_t efs_kv_lane_shard(efs_ino_t ino, uint8_t lane)
{
    uint32_t h = (uint32_t)ino * 2654435761u;
    uint32_t stride = 2u * (h & 0x7FFu) + 1u;
    uint32_t ish = efs_kv_inode_shard(ino);

    return (ish + (uint32_t)lane * stride) & EFS_KV_SHARD_MASK;
}

uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    uint32_t h = 2166136261u;
    int i;

    if (!uuid)
        return 0;
    for (i = 0; i < EFS_OPID_UUID_LEN; i++) {
        h ^= uuid[i];
        h *= 16777619u;
    }
    return h & EFS_KV_SHARD_MASK;
}

int efs_kv_key_alloc(uint32_t shard, uint8_t *out, uint32_t *len)
{
    return start(out, len, shard, EFS_KV_KIND_ALLOC, 0, EFS_KV_KEY_MAX);
}

int efs_kv_key_inode(uint32_t shard, efs_ino_t ino, uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_INODE, 8, EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    return EFS_OK;
}

int efs_kv_key_dentry(uint32_t shard, efs_ino_t parent, const char *name,
                      uint8_t *out, uint32_t *len)
{
    size_t nl;
    int rc;

    if (!name)
        return EFS_ERR_INVAL;
    nl = strlen(name);
    if (nl == 0 || nl >= EFS_MAX_NAME)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_DENTRY, 8 + (uint32_t)nl,
               EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, parent);
    memcpy(out + 11, name, nl);
    return EFS_OK;
}

int efs_kv_key_dentry_prefix(uint32_t shard, efs_ino_t parent,
                             uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_DENTRY, 8, EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, parent);
    return EFS_OK;
}

int efs_kv_key_chunk(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                     uint32_t chunk_index, uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_CHUNK, 8 + 8 + 1 + 4,
                   EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    out[19] = lane;
    be32(out + 20, chunk_index);
    return EFS_OK;
}

int efs_kv_key_lane(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t lane,
                    uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_LANE, 8 + 8 + 1, EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    out[19] = lane;
    return EFS_OK;
}

int efs_kv_key_opid(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    uint32_t epoch, uint8_t *out, uint32_t *len)
{
    int rc;

    if (!uuid)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_OPID, EFS_OPID_UUID_LEN + 4,
               EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    memcpy(out + 3, uuid, EFS_OPID_UUID_LEN);
    be32(out + 3 + EFS_OPID_UUID_LEN, epoch);
    return EFS_OK;
}
