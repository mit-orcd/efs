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

uint8_t efs_kv_dir_lane(const char *name)
{
    uint32_t h = 2166136261u;
    size_t i, n = name ? strlen(name) : 0;

    for (i = 0; i < n; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    return (uint8_t)(h % 64u);
}

uint32_t efs_kv_dentry_shard(efs_ino_t parent, const char *name, uint8_t layout)
{
    if (layout == 0)
        return efs_kv_inode_shard(parent);
    return efs_kv_lane_shard(parent, efs_kv_dir_lane(name));
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

uint32_t efs_kv_mkdir_shard(efs_ino_t parent, const char *name, uint64_t salt)
{
    uint32_t h = 2166136261u;
    uint8_t b[8];
    size_t i, n;

    b[0] = (uint8_t)(parent >> 56);
    b[1] = (uint8_t)(parent >> 48);
    b[2] = (uint8_t)(parent >> 40);
    b[3] = (uint8_t)(parent >> 32);
    b[4] = (uint8_t)(parent >> 24);
    b[5] = (uint8_t)(parent >> 16);
    b[6] = (uint8_t)(parent >> 8);
    b[7] = (uint8_t)parent;
    for (i = 0; i < 8; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    n = name ? strlen(name) : 0;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    for (i = 0; i < 8; i++) {
        h ^= (uint8_t)(salt >> (8 * (7 - i)));
        h *= 16777619u;
    }
    return h & EFS_KV_SHARD_MASK;
}

static int wrap(uint8_t kind, const uint8_t *orig, uint32_t olen, uint8_t *out,
                uint32_t *len)
{
    if (!orig || !out || !len || olen < 3 || olen + 1 > EFS_KV_KEY_MAX)
        return EFS_ERR_INVAL;
    out[0] = orig[0];
    out[1] = orig[1];
    out[2] = kind;
    out[3] = orig[2];
    memcpy(out + 4, orig + 3, olen - 3);
    *len = olen + 1;
    return EFS_OK;
}

static int wrap_txid(uint8_t kind, const uint8_t *orig, uint32_t olen,
                     const uint8_t txid[16], uint8_t *out, uint32_t *len)
{
    uint32_t n;
    int rc;

    if (!txid)
        return EFS_ERR_INVAL;
    rc = wrap(kind, orig, olen, out, &n);
    if (rc != EFS_OK)
        return rc;
    if (n + 16 > EFS_KV_KEY_MAX)
        return EFS_ERR_INVAL;
    memcpy(out + n, txid, 16);
    *len = n + 16;
    return EFS_OK;
}

int efs_kv_key_ver(const uint8_t *orig, uint32_t olen, uint8_t *out, uint32_t *len)
{
    return wrap(EFS_KV_KIND_VER, orig, olen, out, len);
}

int efs_kv_key_intent(const uint8_t *orig, uint32_t olen, uint8_t *out,
                      uint32_t *len)
{
    return wrap(EFS_KV_KIND_INTENT, orig, olen, out, len);
}

int efs_kv_key_guard(const uint8_t *orig, uint32_t olen, const uint8_t txid[16],
                     uint8_t *out, uint32_t *len)
{
    return wrap_txid(EFS_KV_KIND_GUARD, orig, olen, txid, out, len);
}

int efs_kv_key_guard_prefix(const uint8_t *orig, uint32_t olen, uint8_t *out,
                            uint32_t *len)
{
    return wrap(EFS_KV_KIND_GUARD, orig, olen, out, len);
}

int efs_kv_key_reduce(const uint8_t *orig, uint32_t olen, const uint8_t txid[16],
                      uint8_t *out, uint32_t *len)
{
    return wrap_txid(EFS_KV_KIND_REDUCE, orig, olen, txid, out, len);
}

int efs_kv_key_reduce_prefix(const uint8_t *orig, uint32_t olen, uint8_t *out,
                             uint32_t *len)
{
    return wrap(EFS_KV_KIND_REDUCE, orig, olen, out, len);
}

int efs_kv_key_decision(uint32_t shard, const uint8_t txid[16], uint8_t *out,
                        uint32_t *len)
{
    int rc;

    if (!txid)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_DECISION, 16, EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    memcpy(out + 3, txid, 16);
    return EFS_OK;
}

int efs_kv_key_dseq(uint32_t shard, efs_ino_t dir, uint8_t lane, uint8_t *out,
                    uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_DSEQ, 8 + 1, EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, dir);
    out[11] = lane;
    return EFS_OK;
}

int efs_kv_key_session(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                       uint8_t *out, uint32_t *len)
{
    int rc;

    if (!uuid)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_SESSION, EFS_OPID_UUID_LEN,
               EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    memcpy(out + 3, uuid, EFS_OPID_UUID_LEN);
    return EFS_OK;
}

int efs_kv_key_sess_local(uint32_t shard, const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint8_t *out, uint32_t *len)
{
    int rc;

    if (!uuid)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_SESS_LOCAL, EFS_OPID_UUID_LEN,
               EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    memcpy(out + 3, uuid, EFS_OPID_UUID_LEN);
    return EFS_OK;
}

int efs_kv_key_lease(uint32_t shard, efs_ino_t ino, uint64_t gen,
                     const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
                     uint8_t *out, uint32_t *len)
{
    int rc;

    if (!uuid)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_LEASE,
               8 + 8 + EFS_OPID_UUID_LEN + 4, EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    memcpy(out + 19, uuid, EFS_OPID_UUID_LEN);
    be32(out + 19 + EFS_OPID_UUID_LEN, epoch);
    return EFS_OK;
}

int efs_kv_key_lease_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                            uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_LEASE, 8 + 8, EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    return EFS_OK;
}

int efs_kv_key_lease_shard_prefix(uint32_t shard, uint8_t *out, uint32_t *len)
{
    return start(out, len, shard, EFS_KV_KIND_LEASE, 0, EFS_KV_KEY_MAX);
}

int efs_kv_key_lock(uint32_t shard, efs_ino_t ino, uint64_t gen, uint8_t domain,
                    uint64_t lo, uint64_t hi, uint8_t owner_kind,
                    uint64_t owner_id, const uint8_t uuid[EFS_OPID_UUID_LEN],
                    uint32_t epoch, uint8_t *out, uint32_t *len)
{
    int rc;

    if (!uuid)
        return EFS_ERR_INVAL;
    rc = start(out, len, shard, EFS_KV_KIND_LOCK,
               8 + 8 + 1 + 8 + 8 + 1 + 8 + EFS_OPID_UUID_LEN + 4,
               EFS_KV_KEY_MAX);
    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    out[19] = domain;
    be64(out + 20, lo);
    be64(out + 28, hi);
    out[36] = owner_kind;
    be64(out + 37, owner_id);
    memcpy(out + 45, uuid, EFS_OPID_UUID_LEN);
    be32(out + 45 + EFS_OPID_UUID_LEN, epoch);
    return EFS_OK;
}

int efs_kv_key_lock_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                           uint8_t domain, uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_LOCK, 8 + 8 + 1, EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    out[19] = domain;
    return EFS_OK;
}

int efs_kv_key_lock_file_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                                uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_LOCK, 8 + 8, EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    return EFS_OK;
}

int efs_kv_key_lock_shard_prefix(uint32_t shard, uint8_t *out, uint32_t *len)
{
    return start(out, len, shard, EFS_KV_KIND_LOCK, 0, EFS_KV_KEY_MAX);
}

int efs_kv_key_append_cur(uint32_t shard, efs_ino_t ino, uint64_t gen,
                          uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_APPEND_CUR, 8 + 8,
                   EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    return EFS_OK;
}

int efs_kv_key_append_rsv(uint32_t shard, efs_ino_t ino, uint64_t gen,
                          uint64_t off, uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_APPEND_RSV, 8 + 8 + 8,
                   EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    be64(out + 19, off);
    return EFS_OK;
}

int efs_kv_key_append_rsv_prefix(uint32_t shard, efs_ino_t ino, uint64_t gen,
                                 uint8_t *out, uint32_t *len)
{
    int rc = start(out, len, shard, EFS_KV_KIND_APPEND_RSV, 8 + 8,
                   EFS_KV_KEY_MAX);

    if (rc != EFS_OK)
        return rc;
    be64(out + 3, ino);
    be64(out + 11, gen);
    return EFS_OK;
}

int efs_kv_key_unwrap(const uint8_t *wrapk, uint32_t wlen, uint8_t *orig,
                      uint32_t *olen)
{
    if (!wrapk || !orig || !olen || wlen < 4)
        return EFS_ERR_INVAL;
    orig[0] = wrapk[0];
    orig[1] = wrapk[1];
    orig[2] = wrapk[3];
    memcpy(orig + 3, wrapk + 4, wlen - 4);
    *olen = wlen - 1;
    return EFS_OK;
}
