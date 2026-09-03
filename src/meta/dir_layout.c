#include "efs/dir_layout.h"
#include "efs/meta_apply.h"
#include "efs/kv_key.h"
#include <string.h>
#include <sys/stat.h>

struct hit {
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen;
    uint8_t val[EFS_META_DENT_BYTES];
    uint32_t vlen;
    char name[EFS_MAX_NAME];
    int found;
};

static int first_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct hit *h = user;
    uint32_t nlen;

    if (h->found)
        return 1;
    if (klen < 11 || vlen < EFS_META_DENT_BYTES)
        return 0;
    nlen = klen - 11;
    if (nlen == 0 || nlen >= EFS_MAX_NAME)
        return 0;
    memcpy(h->key, key, klen);
    h->klen = klen;
    memcpy(h->val, val, EFS_META_DENT_BYTES);
    h->vlen = EFS_META_DENT_BYTES;
    memcpy(h->name, key + 11, nlen);
    h->name[nlen] = 0;
    h->found = 1;
    return 1;
}

static int put_inode(struct efs_kv *kv, const struct efs_meta_row *r)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_INO_BYTES];
    uint32_t klen = 0;
    int rc;

    rc = efs_kv_key_inode(efs_kv_inode_shard(r->ino), r->ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_pack_inode(r, val, sizeof(val));
    if (rc != EFS_OK)
        return rc;
    return efs_kv_put(kv, key, klen, val, EFS_META_INO_BYTES);
}

int efs_meta_dir_begin_split(struct efs_kv *kv, efs_ino_t dir)
{
    struct efs_meta_row row;
    int rc;

    if (!kv || dir == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if ((row.mode & S_IFMT) != S_IFDIR)
        return EFS_ERR_INVAL;
    if (row.layout == EFS_META_LAYOUT_SPLITTING)
        return EFS_OK;
    if (row.layout != EFS_META_LAYOUT_LOCAL)
        return EFS_ERR_INVAL;
    row.layout = EFS_META_LAYOUT_SPLITTING;
    row.layout_epoch++;
    return put_inode(kv, &row);
}

int efs_meta_dir_migrate_one(struct efs_kv *kv, efs_ino_t dir)
{
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct hit h;
    struct efs_kv_item it[2];
    uint8_t pref[EFS_KV_KEY_MAX], hk[EFS_KV_KEY_MAX], hv[EFS_META_DENT_BYTES];
    uint32_t plen = 0, hklen = 0, hvlen;
    uint32_t psh, hsh, n = 0;
    int rc;

    if (!kv || dir == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.layout != EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_INVAL;
    psh = efs_kv_inode_shard(dir);
    rc = efs_kv_key_dentry_prefix(psh, dir, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    memset(&h, 0, sizeof(h));
    rc = efs_kv_scan_prefix(kv, pref, plen, first_cb, &h);
    if (rc != EFS_OK && rc != 1)
        return rc;
    if (!h.found)
        return EFS_ERR_NOT_FOUND;
    rc = efs_meta_unpack_dentry(h.val, h.vlen, &dent);
    if (rc != EFS_OK)
        return rc;
    hsh = efs_kv_dentry_shard(dir, h.name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(hsh, dir, h.name, hk, &hklen);
    if (rc != EFS_OK)
        return rc;
    hvlen = sizeof(hv);
    rc = efs_kv_get(kv, hk, hklen, hv, &hvlen);
    memset(it, 0, sizeof(it));
    if (rc == EFS_ERR_NOT_FOUND) {
        it[n].op = EFS_KV_PUT;
        it[n].key = hk;
        it[n].klen = hklen;
        it[n].val = h.val;
        it[n].vlen = h.vlen;
        n++;
    } else if (rc != EFS_OK) {
        return rc;
    }
    it[n].op = EFS_KV_DEL;
    it[n].key = h.key;
    it[n].klen = h.klen;
    n++;
    return efs_kv_batch(kv, it, n);
}

int efs_meta_dir_finish_hashed(struct efs_kv *kv, efs_ino_t dir)
{
    struct efs_meta_row row;
    struct hit h;
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    if (!kv || dir == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.layout == EFS_META_LAYOUT_HASHED)
        return EFS_OK;
    if (row.layout != EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_dentry_prefix(efs_kv_inode_shard(dir), dir, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    memset(&h, 0, sizeof(h));
    rc = efs_kv_scan_prefix(kv, pref, plen, first_cb, &h);
    if (rc != EFS_OK && rc != 1)
        return rc;
    if (h.found)
        return EFS_ERR_BUSY;
    row.layout = EFS_META_LAYOUT_HASHED;
    return put_inode(kv, &row);
}
