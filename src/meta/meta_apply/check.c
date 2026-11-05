/* meta_apply/check.c: the whole-store consistency check. */

#include "apply_internal.h"
#include "rows.h"
#include "publish.h"

struct check_scan {
    struct efs_kv *kv;
    int rc;
    int saw_root;
};

static int check_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct check_scan *c = user;
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    if (klen < 3)
        return 0;
    if (key[2] == EFS_KV_KIND_INODE) {
        rc = unpack_inode(val, vlen, &r);
        if (rc != EFS_OK) {
            c->rc = rc;
            return 1;
        }
        if (r.ino == EFS_ROOT_INO)
            c->saw_root = 1;
        if (efs_kv_inode_shard(r.ino) != (((uint32_t)key[0] << 8) | key[1])) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_TRUNC_PENDING) {
        if (klen != 11 || !rd64(key + 3) || vlen != 8 || !rd64(val) ||
            efs_kv_anchor_shard(efs_kv_inode_shard(rd64(key + 3))) !=
                (((uint32_t)key[0] << 8) | key[1])) {
            c->rc = EFS_ERR_PROTO; return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_APPEND_RECEIPT ||
        key[2] == EFS_KV_KIND_TRUNCATE_RECEIPT) {
        struct efs_opid_window window;
        if (klen != 31 || !rd64(key + 3) || !efs_load_be32(key + 27) ||
            efs_kv_inode_shard(rd64(key + 3)) != (((uint32_t)key[0] << 8) | key[1]) ||
            efs_opid_window_unpack(&window, val, vlen) != EFS_OK ||
            memcmp(window.client_uuid, key + 11, EFS_OPID_UUID_LEN) ||
            window.session_epoch != efs_load_be32(key + 27)) {
            c->rc = EFS_ERR_PROTO; return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_PUBLICATION_FLOOR) {
        if(klen!=43 || !rd64(key+3) || !rd64(key+11) || !efs_load_be32(key+39) ||
           efs_kv_lane_shard(rd64(key+3),efs_load_be32(key+19)%EFS_META_LANES)!=
             (((uint32_t)key[0]<<8)|key[1]) || vlen!=12 || efs_load_be32(val)!=1 || !rd64(val+4)) {
            c->rc=EFS_ERR_PROTO;return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_PUBLICATION) {
        uint32_t ci = klen == 51 ? efs_load_be32(key + 19) : 0;
        if (klen != 51 || !rd64(key + 3) || !rd64(key + 11) ||
            efs_kv_lane_shard(rd64(key + 3), ci % EFS_META_LANES) !=
                (((uint32_t)key[0] << 8) | key[1]) ||
            !efs_load_be32(key + 39) || !rd64(key + 43) ||
            !publication_receipt_valid(val,vlen)) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_LANE_AUTHORITY) {
        uint32_t cs; uint64_t epoch, floor;
        if (klen != 20 || !rd64(key + 3) || !rd64(key + 11) || key[19] >= EFS_META_LANES ||
            efs_kv_lane_shard(rd64(key + 3), key[19]) != (((uint32_t)key[0] << 8) | key[1]) ||
            unpack_lane_authority(val, vlen, &cs, &epoch, &floor) != EFS_OK) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] == EFS_KV_KIND_CONTENT_FENCE) {
        struct efs_fence_history history;
        uint32_t shard;
        uint8_t authority;
        if (klen != 20 || !rd64(key + 3) || !rd64(key + 11) ||
            key[19] > EFS_META_FENCE_INODE) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        authority = key[19];
        shard = authority == EFS_META_FENCE_INODE ?
                efs_kv_inode_shard(rd64(key + 3)) :
                efs_kv_lane_shard(rd64(key + 3), authority);
        if (shard != (((uint32_t)key[0] << 8) | key[1]) ||
            efs_meta_unpack_fence_history(val, vlen, &history) != EFS_OK) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] != EFS_KV_KIND_DENTRY)
        return 0;
    rc = unpack_dentry(val, vlen, &d);
    if (rc != EFS_OK) {
        c->rc = rc;
        return 1;
    }
    if (d.type == EFS_META_DENT_TOMBSTONE || d.ino == 0)
        return 0;
    rc = efs_meta_apply_get_inode(c->kv, d.ino, &r);
    if (rc == EFS_ERR_NOT_FOUND) {
        c->rc = EFS_ERR_IO; /* I9 */
        return 1;
    }
    if (rc != EFS_OK) {
        c->rc = rc;
        return 1;
    }
    return 0;
}

int efs_meta_apply_check(struct efs_kv *kv)
{
    struct check_scan c = { .kv = kv, .rc = EFS_OK, .saw_root = 0 };
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_kv_scan(kv, check_cb, &c);
    if (c.rc != EFS_OK)
        return c.rc;
    if (rc != EFS_OK)
        return rc;
    if (!c.saw_root)
        return EFS_ERR_PROTO;
    return EFS_OK;
}
