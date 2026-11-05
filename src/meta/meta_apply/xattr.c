/* meta_apply/xattr.c: extended attributes. */

#include "apply_internal.h"
#include "xattr.h"
#include "rows.h"

/* Blob: u32 count, then count times u16 nlen, u32 vlen, name, value.
 * One key per inode. Empty means the key is absent, not a zero count. */

static int xattr_check_blob(const uint8_t *b, uint32_t n, uint32_t *count)
{
    uint32_t c, pos, i;

    if (n == 0) {
        *count = 0;
        return EFS_OK;
    }
    if (n < 4 || n > EFS_XATTR_BLOB_MAX)
        return EFS_ERR_PROTO;
    c = efs_load_be32(b);
    pos = 4;
    for (i = 0; i < c; i++) {
        uint16_t nl;
        uint32_t vl, step;

        if (pos > n || n - pos < 6)
            return EFS_ERR_PROTO;
        nl = rd16(b + pos);
        vl = efs_load_be32(b + pos + 2);
        if (nl == 0 || nl > EFS_XATTR_NAME_MAX || vl > EFS_XATTR_VALUE_MAX)
            return EFS_ERR_PROTO;
        step = 6u + (uint32_t)nl + vl;
        if (step < 6 || n - pos < step)
            return EFS_ERR_PROTO;
        pos += step;
    }
    if (pos != n)
        return EFS_ERR_PROTO;
    *count = c;
    return EFS_OK;
}

static int xattr_load(struct efs_kv *kv, efs_ino_t ino, uint8_t *k, uint32_t *kl,
                      uint8_t *blob, uint32_t *blen)
{
    int rc;

    rc = efs_kv_key_xattr(efs_kv_inode_shard(ino), ino, k, kl);
    if (rc != EFS_OK)
        return rc;
    *blen = EFS_XATTR_BLOB_MAX;
    rc = efs_kv_get(kv, k, *kl, blob, blen);
    if (rc == EFS_ERR_NOT_FOUND) {
        *blen = 0;
        return EFS_OK;
    }
    return rc;
}

static int blob_put(uint8_t *dst, uint32_t cap, uint32_t *dp, const uint8_t *name,
                    uint16_t nlen, const uint8_t *val, uint32_t vlen)
{
    uint32_t need = 6u + (uint32_t)nlen + vlen;

    if (need < 6 || *dp > cap || cap - *dp < need)
        return EFS_ERR_INVAL;
    be16(dst + *dp, nlen);
    efs_store_be32(dst + *dp + 2, vlen);
    memcpy(dst + *dp + 6, name, nlen);
    if (vlen)
        memcpy(dst + *dp + 6 + nlen, val, vlen);
    *dp += need;
    return EFS_OK;
}

/* Fill one DEL when this inode has an xattr blob. A miss is not a
 * tombstone — every unlink would otherwise plant a key the file never
 * had. *added is 1 when it was filled. */
int xattr_del_item(struct efs_kv *kv, efs_ino_t ino, uint8_t *k,
                          uint32_t *kl, struct efs_kv_item *it, int *added)
{
    uint32_t pn = 0;
    int rc;

    *added = 0;
    rc = efs_kv_key_xattr(efs_kv_inode_shard(ino), ino, k, kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, *kl, NULL, &pn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_ERR_INVAL && rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(*it));
    it->op = EFS_KV_DEL;
    it->key = k;
    it->klen = *kl;
    *added = 1;
    return EFS_OK;
}

static int xattr_name_ok(const uint8_t *name, uint16_t nlen, const uint8_t *val,
                         uint32_t vlen)
{
    uint16_t i;

    if (!name || nlen == 0 || nlen > EFS_XATTR_NAME_MAX || vlen > EFS_XATTR_VALUE_MAX)
        return EFS_ERR_INVAL;
    if (vlen && !val)
        return EFS_ERR_INVAL;
    for (i = 0; i < nlen; i++) {
        if (name[i] == 0)
            return EFS_ERR_INVAL;
    }
    return EFS_OK;
}

int efs_meta_apply_xattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                         uint8_t op, uint32_t flags, const uint8_t *name,
                         uint16_t nlen, const uint8_t *val, uint32_t vlen)
{
    struct efs_meta_row row;
    uint8_t blob[EFS_XATTR_BLOB_MAX], neu[EFS_XATTR_BLOB_MAX];
    uint8_t k_xa[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL];
    uint32_t kl = 0, ki = 0, blen = 0, count = 0, pos, i, dp, nc;
    struct efs_kv_item it[2];
    int rc, replaced = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (op != EFS_XATTR_SET && op != EFS_XATTR_REMOVE)
        return EFS_ERR_INVAL;
    if ((flags & ~(EFS_XATTR_CREATE | EFS_XATTR_REPLACE)) != 0)
        return EFS_ERR_INVAL;
    if ((flags & EFS_XATTR_CREATE) && (flags & EFS_XATTR_REPLACE))
        return EFS_ERR_INVAL;
    rc = xattr_name_ok(name, nlen, val, vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = xattr_load(kv, ino, k_xa, &kl, blob, &blen);
    if (rc != EFS_OK)
        return rc;
    rc = xattr_check_blob(blob, blen, &count);
    if (rc != EFS_OK)
        return rc;
    dp = 4;
    nc = 0;
    pos = blen ? 4 : 0;
    for (i = 0; i < count; i++) {
        uint16_t nl = rd16(blob + pos);
        uint32_t vl = efs_load_be32(blob + pos + 2);
        const uint8_t *nm = blob + pos + 6;
        const uint8_t *vv = nm + nl;
        uint32_t step = 6u + (uint32_t)nl + vl;
        int match = nl == nlen && memcmp(nm, name, nlen) == 0;

        if (match && op == EFS_XATTR_SET) {
            if (flags & EFS_XATTR_CREATE)
                return EFS_ERR_EXIST;
            if (vl == vlen && (vlen == 0 || memcmp(vv, val, vlen) == 0))
                return EFS_OK;
            rc = blob_put(neu, EFS_XATTR_BLOB_MAX, &dp, name, nlen, val, vlen);
            if (rc != EFS_OK)
                return rc;
            nc++;
            replaced = 1;
        } else if (match && op == EFS_XATTR_REMOVE) {
            replaced = 1;
        } else {
            rc = blob_put(neu, EFS_XATTR_BLOB_MAX, &dp, nm, nl, vv, vl);
            if (rc != EFS_OK)
                return rc;
            nc++;
        }
        pos += step;
    }
    if (op == EFS_XATTR_SET && !replaced) {
        if (flags & EFS_XATTR_REPLACE)
            return EFS_ERR_NODATA;
        rc = blob_put(neu, EFS_XATTR_BLOB_MAX, &dp, name, nlen, val, vlen);
        if (rc != EFS_OK)
            return rc;
        nc++;
    }
    if (op == EFS_XATTR_REMOVE && !replaced)
        return EFS_ERR_NODATA;
    efs_store_be32(neu, nc);
    row.base_ctime = max_u64(row.base_ctime, now);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    pack_inode(v_ino, &row);
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_PUT;
    it[0].key = k_ino;
    it[0].klen = ki;
    it[0].val = v_ino;
    it[0].vlen = INO_VAL;
    if (nc == 0) {
        it[1].op = EFS_KV_DEL;
        it[1].key = k_xa;
        it[1].klen = kl;
    } else {
        it[1].op = EFS_KV_PUT;
        it[1].key = k_xa;
        it[1].klen = kl;
        it[1].val = neu;
        it[1].vlen = dp;
    }
    return meta_write_batch(kv, it, 2);
}

int efs_meta_xattr_get(struct efs_kv *kv, efs_ino_t ino, const uint8_t *name,
                       uint16_t nlen, uint8_t *val, uint32_t *vlen)
{
    struct efs_meta_row row;
    uint8_t blob[EFS_XATTR_BLOB_MAX], k[EFS_KV_KEY_MAX];
    uint32_t kl = 0, blen = 0, count = 0, pos, i;
    int rc;

    if (!kv || !vlen || ino == 0)
        return EFS_ERR_INVAL;
    rc = xattr_name_ok(name, nlen, NULL, 0);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.ino != ino)
        return EFS_ERR_PROTO;
    rc = xattr_load(kv, ino, k, &kl, blob, &blen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, k, kl, xattr_check_blob(blob, blen, &count));
    if (rc != EFS_OK)
        return rc;
    pos = blen ? 4 : 0;
    for (i = 0; i < count; i++) {
        uint16_t nl = rd16(blob + pos);
        uint32_t vl = efs_load_be32(blob + pos + 2);
        const uint8_t *nm = blob + pos + 6;
        const uint8_t *vv = nm + nl;

        if (nl == nlen && memcmp(nm, name, nlen) == 0) {
            if (*vlen < vl) {
                *vlen = vl;
                return EFS_ERR_INVAL;
            }
            if (vl && val)
                memcpy(val, vv, vl);
            *vlen = vl;
            return EFS_OK;
        }
        pos += 6u + (uint32_t)nl + vl;
    }
    return EFS_ERR_NODATA;
}

int efs_meta_xattr_list(struct efs_kv *kv, efs_ino_t ino, uint8_t *buf,
                        uint32_t *len)
{
    struct efs_meta_row row;
    uint8_t blob[EFS_XATTR_BLOB_MAX], k[EFS_KV_KEY_MAX];
    uint32_t kl = 0, blen = 0, count = 0, pos, i, dp, cap;
    int rc;

    if (!kv || !len || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.ino != ino)
        return EFS_ERR_PROTO;
    rc = xattr_load(kv, ino, k, &kl, blob, &blen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, k, kl, xattr_check_blob(blob, blen, &count));
    if (rc != EFS_OK)
        return rc;
    cap = buf ? *len : 0;
    dp = 0;
    pos = blen ? 4 : 0;
    for (i = 0; i < count; i++) {
        uint16_t nl = rd16(blob + pos);
        uint32_t vl = efs_load_be32(blob + pos + 2);
        const uint8_t *nm = blob + pos + 6;
        uint32_t piece = (uint32_t)nl + 1;

        if (buf && dp + piece <= cap) {
            memcpy(buf + dp, nm, nl);
            buf[dp + nl] = 0;
        }
        dp += piece;
        pos += 6u + (uint32_t)nl + vl;
    }
    if (buf && dp > cap) {
        *len = dp;
        return EFS_ERR_INVAL;
    }
    *len = dp;
    return EFS_OK;
}
