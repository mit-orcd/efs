/* meta_apply/reclaim.c: inode reclaim and orphan reclaim. */

#include "apply_internal.h"
#include "rows.h"
#include "xattr.h"

int efs_meta_apply_reclaim(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint8_t k_xa[EFS_KV_KEY_MAX];
    uint32_t ki = 0, kr = 0, kxa = 0;
    struct efs_kv_item it[4];
    uint8_t k_orphan[EFS_KV_KEY_MAX];
    int rc, held, xa = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.nlink != 0)
        return EFS_ERR_BUSY;
    held = efs_lease_any(kv, ino, row.generation);
    if (held < 0)
        return held;
    if (held)
        return EFS_ERR_BUSY;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_txn_key_busy(kv, k_ino, ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),
                             ino, k_reap, &kr);
    if (rc != EFS_OK)
        return rc;
    /* Row delete and reap marker are one batch: the marker is the reaper's
     * only record of the dead file's lanes (L7), so it can never be lost
     * to a crash between the two writes. Append cursor/reservation records
     * are cleaned by REAP_DONE, keeping this hot entry small. */
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_DEL;
    it[0].key = k_ino;
    it[0].klen = ki;
    efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
    it[1].op = EFS_KV_PUT;
    it[1].key = k_reap;
    it[1].klen = kr;
    it[1].val = v_reap;
    it[1].vlen = EFS_META_REAP_VAL;
    rc = xattr_del_item(kv, ino, k_xa, &kxa, &it[2], &xa);
    if (rc != EFS_OK)
        return rc;
    memcpy(k_orphan, k_reap, kr); k_orphan[2] = EFS_KV_KIND_ORPHAN;
    it[xa ? 3 : 2] = (struct efs_kv_item){EFS_KV_DEL, k_orphan, kr, NULL, 0};
    return meta_write_batch(kv, it, xa ? 4 : 3);
}

/* Unlike a REAP marker, this index never authorizes fragment deletion.
 * Closing a lease can precede RESOLVE's nlink decrement; recheck after all
 * pending namespace transactions settle, on the inode's own Raft group. */
int efs_meta_apply_orphan_reclaim(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], value[8];
    uint32_t len = 0, vl = sizeof(value);
    if (!kv || ino <= EFS_ROOT_INO || !gen) return EFS_ERR_INVAL;
    int rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_OK && row.generation == gen && !row.nlink)
        return efs_meta_apply_reclaim(kv, ino);
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)), ino, key, &len);
    if (rc != EFS_OK) return rc;
    key[2] = EFS_KV_KIND_ORPHAN;
    rc = efs_kv_get(kv, key, len, value, &vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc != EFS_OK) return rc;
    if (vl != sizeof(value)) return EFS_ERR_PROTO;
    if (rd64(value) != gen) return EFS_ERR_STALE;
    rc = meta_write_del(kv, key, len);
    return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
}
