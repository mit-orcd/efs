#include "efs/dir_layout.h"
#include "efs/dir_spread.h"
#include "efs/meta_apply.h"
#include "efs/kv_key.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

uint32_t efs_dir_spread_min(void)
{
    const char *e = getenv("EFS_DIR_SPREAD_MIN");
    unsigned long v;
    char *end = NULL;

    if (e && *e) {
        v = strtoul(e, &end, 10);
        if (end != e && v > 0 && v <= UINT32_MAX)
            return (uint32_t)v;
    }
    return EFS_DIR_SPREAD_MIN;
}

void efs_meta_dir_note_entry(struct efs_meta_row *row, int delta)
{
    if (!row || row->layout != EFS_META_LAYOUT_LOCAL)
        return;
    if (delta > 0) {
        if (row->nents < UINT32_MAX)
            row->nents++;
    } else if (delta < 0 && row->nents > 0) {
        row->nents--;
    }
    if (delta > 0 && row->nents > efs_dir_spread_min()) {
        row->layout = EFS_META_LAYOUT_SPLITTING;
        row->layout_epoch++;
        efs_dir_spread_note(row->ino);
    }
}

struct hit {
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen;
    uint8_t val[EFS_META_DENT_BYTES];
    uint32_t vlen;
    char name[EFS_MAX_NAME];
    int found;
    int saw_lane0; /* a name already in its final place; not migration work */
};

/* Finds the first dentry in the pre-split local range that actually has
 * somewhere to move. Dir lane 0 is the directory's own inode shard, so a
 * lane-0 name's hashed key is the local key it already occupies: there is
 * nothing to copy, and "moving" it would delete the only copy. Those names
 * stay in the local range for the directory's lifetime, which is why
 * "the local range is empty" is not the test for a finished migration. */
static int first_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct hit *h = user;
    uint32_t nlen;
    char name[EFS_MAX_NAME];

    if (h->found)
        return 1;
    if (klen < 11 || vlen < EFS_META_DENT_BYTES)
        return 0;
    nlen = klen - 11;
    if (nlen == 0 || nlen >= EFS_MAX_NAME)
        return 0;
    memcpy(name, key + 11, nlen);
    name[nlen] = 0;
    if (efs_kv_dir_lane(name) == 0) {
        h->saw_lane0 = 1;
        return 0;
    }
    memcpy(h->key, key, klen);
    h->klen = klen;
    memcpy(h->val, val, EFS_META_DENT_BYTES);
    h->vlen = EFS_META_DENT_BYTES;
    memcpy(h->name, name, nlen + 1);
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
    if (row.layout == EFS_META_LAYOUT_SPLITTING) {
        efs_dir_spread_note(dir);
        return EFS_OK;
    }
    if (row.layout != EFS_META_LAYOUT_LOCAL)
        return EFS_ERR_INVAL;
    row.layout = EFS_META_LAYOUT_SPLITTING;
    row.layout_epoch++;
    rc = put_inode(kv, &row);
    if (rc == EFS_OK)
        efs_dir_spread_note(dir);
    return rc;
}

int efs_meta_dir_migrate_peek(struct efs_kv *kv, efs_ino_t dir, char *name,
                              uint32_t nmax, uint32_t *hsh, int *saw_lane0)
{
    struct efs_meta_row row;
    struct hit h;
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0, psh;
    int rc;

    if (saw_lane0)
        *saw_lane0 = 0;
    if (hsh)
        *hsh = 0;
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
    if (saw_lane0)
        *saw_lane0 = h.saw_lane0;
    if (!h.found)
        return EFS_ERR_NOT_FOUND;
    if (name) {
        if (nmax == 0)
            return EFS_ERR_INVAL;
        strncpy(name, h.name, nmax - 1);
        name[nmax - 1] = 0;
    }
    if (hsh)
        *hsh = efs_kv_dentry_shard(dir, h.name, EFS_META_LAYOUT_HASHED);
    return EFS_OK;
}

int efs_meta_dir_migrate_one(struct efs_kv *kv, efs_ino_t dir)
{
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct hit h;
    struct efs_kv_item it[3];
    uint8_t pref[EFS_KV_KEY_MAX], hk[EFS_KV_KEY_MAX], hv[EFS_META_DENT_BYTES];
    uint8_t rk[EFS_KV_KEY_MAX], rv[EFS_META_INO_BYTES];
    uint32_t plen = 0, hklen = 0, hvlen, rklen = 0;
    uint32_t psh, hsh, n = 0;
    uint64_t bit;
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
    if (!h.found) {
        /* Nothing left to move. Registering lane 0 is the last piece of
         * migration work: those names never passed through the code below,
         * so nothing else has recorded the lane they occupy, and at HASHED
         * the bitmap is all readdir has to find them by. */
        if (h.saw_lane0 && (row.used_shards & 1ull) == 0) {
            row.used_shards |= 1ull;
            return put_inode(kv, &row);
        }
        return EFS_ERR_NOT_FOUND;
    }
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
    /* I8: hashed live/tombstone already at this key wins; skip the PUT.
     * Cross-group leftovers are moved by the host/sim txn so the PUT
     * lands in the dest group's log. This single-KV batch is for the
     * same-group leftover and for tests that share one KV. */
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
    /* First use of this dir lane has to be recorded, in the same batch as the
     * move. The bitmap is what bounds readdir and dir stat to the lanes a
     * directory actually occupies, so a lane the migrator populated silently
     * would be a lane those scans never visit. */
    bit = 1ull << efs_kv_dir_lane(h.name);
    if ((row.used_shards & bit) == 0) {
        row.used_shards |= bit;
        rc = efs_kv_key_inode(psh, dir, rk, &rklen);
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_pack_inode(&row, rv, sizeof(rv));
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = rk;
        it[n].klen = rklen;
        it[n].val = rv;
        it[n].vlen = EFS_META_INO_BYTES;
        n++;
    }
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
    /* Idempotent with the migrator's own registration, so finishing does not
     * depend on the migrator having been the one to run last. */
    if (h.saw_lane0)
        row.used_shards |= 1ull;
    return put_inode(kv, &row);
}
