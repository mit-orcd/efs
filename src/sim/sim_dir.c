/* Directory layout-epoch Raft cmds (§10 step 10 / I8). */
#include "sim_internal.h"
#include "efs/dir_layout.h"
#include "efs/dir_spread.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/txn.h"
#include <string.h>
#include <sys/stat.h>

static void wr64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

int sim_dir_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                  uint32_t clen, uint64_t index)
{
    efs_ino_t dir = 0;
    int rc = EFS_ERR_PROTO;

    if (!s || !s->disk || !cmd || clen < 10 || cmd[0] != EFS_MD_CMD_DIR)
        goto done;
    dir = rd64(cmd + 2);
    switch (cmd[1]) {
    case EFS_MD_DIR_BEGIN:
        rc = efs_meta_dir_begin_split(s->disk, dir);
        break;
    case EFS_MD_DIR_MIGRATE:
        rc = efs_meta_dir_migrate_one(s->disk, dir);
        break;
    case EFS_MD_DIR_FINISH:
        rc = efs_meta_dir_finish_hashed(s->disk, dir);
        break;
    default:
        break;
    }
done:
    sim_note_apply(s, group, index, rc, dir);
    return EFS_OK;
}

static int propose_dir(struct efs_sim *sim, uint8_t kind, efs_ino_t dir)
{
    uint8_t cmd[10];
    uint32_t shard;

    cmd[0] = EFS_MD_CMD_DIR;
    cmd[1] = kind;
    wr64(cmd + 2, dir);
    shard = efs_kv_inode_shard(dir);
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, 10);
}

int efs_sim_dir_begin_split(struct efs_sim *sim, efs_ino_t dir)
{
    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    return propose_dir(sim, EFS_MD_DIR_BEGIN, dir);
}

/* Cross-group leftover: PUT hashed + DEL local + used_shards in one txn
 * so the hashed dentry is in the dest group's log (I8: skip PUT if the
 * hashed key already exists). Same-group leftovers stay a DIR_MIGRATE
 * apply. */
static int migrate_txn(struct efs_sim *sim, efs_ino_t dir, const char *name,
                       uint32_t hsh)
{
    struct efs_kv *pkv, *hkv;
    struct efs_meta_row row;
    struct efs_meta_dentry dent;
    struct efs_txid t;
    struct efs_txn_parts p;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES], v_dseq[8];
    uint8_t loc_buf[EFS_META_DENT_BYTES], sb[8];
    uint32_t kl = 0, kh = 0, ki = 0, ks = 0, locn, sn;
    uint32_t psh;
    uint64_t loc_ver = 0, hash_ver = 0, ino_ver = 0, sver = 0, seq = 0, bit;
    uint8_t lane;
    int rc, i, gr, stamp_ino = 0;
    uint32_t coord;

    psh = efs_kv_inode_shard(dir);
    lane = efs_kv_dir_lane(name);
    bit = 1ull << lane;
    rc = sim_txn_read_kv(sim, psh, &pkv);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_read_kv(sim, hsh, &hkv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(pkv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_dentry(psh, dir, name, k_loc, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, dir, name, k_hash, &kh);
    if (rc != EFS_OK)
        return rc;
    locn = sizeof(loc_buf);
    rc = efs_kv_get(pkv, k_loc, kl, loc_buf, &locn);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_unpack_dentry(loc_buf, locn, &dent);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
    if (rc != EFS_OK)
        return rc;
    if ((row.used_shards & bit) == 0) {
        row.used_shards |= bit;
        stamp_ino = 1;
        rc = efs_kv_key_inode(psh, dir, k_ino, &ki);
        if (rc == EFS_OK)
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_kv_key_dseq(hsh, dir, lane, k_dseq, &ks);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(pkv, k_loc, kl, &loc_ver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(hkv, k_hash, kh, &hash_ver);
    if (rc == EFS_OK && stamp_ino)
        rc = efs_txn_ver_get(pkv, k_ino, ki, &ino_ver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(hkv, k_dseq, ks, &sver);
    if (rc != EFS_OK)
        return rc;
    sn = 8;
    gr = efs_kv_get(hkv, k_dseq, ks, sb, &sn);
    seq = (gr == EFS_OK && sn >= 8) ? rd64(sb) : 0;
    wr64(v_dseq, seq + 1);
    memset(&p, 0, sizeof(p));
    rc = sim_txn_parts_add(&p, psh);
    if (rc == EFS_OK)
        rc = sim_txn_parts_add(&p, hsh);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < p.n; i++) {
        rc = sim_sess_ensure(sim, 0, p.shard[i]);
        if (rc != EFS_OK)
            return rc;
    }
    sim_txn_fill_txid(sim, &t);
    for (i = 0; i < p.n; i++) {
        uint32_t sh = p.shard[i];

        if (sh == psh) {
            rc = sim_txn_propose_prep(sim, psh, EFS_TXN_EXCL, &t, &p, k_loc, kl,
                                      loc_ver, EFS_TXN_DEL, NULL, 0, NULL);
            if (rc != EFS_OK)
                goto fail;
            if (stamp_ino) {
                rc = sim_txn_propose_prep(sim, psh, EFS_TXN_EXCL, &t, &p, k_ino,
                                          ki, ino_ver, EFS_TXN_PUT, v_ino,
                                          sizeof(v_ino), NULL);
                if (rc != EFS_OK)
                    goto fail;
            }
        }
        if (sh == hsh) {
            rc = sim_txn_propose_prep(sim, hsh, EFS_TXN_EXCL, &t, &p, k_hash,
                                      kh, hash_ver, EFS_TXN_PUT, v_dent,
                                      sizeof(v_dent), NULL);
            if (rc != EFS_OK)
                goto fail;
            rc = sim_txn_propose_prep(sim, hsh, EFS_TXN_EXCL, &t, &p, k_dseq,
                                      ks, sver, EFS_TXN_PUT, v_dseq, 8, NULL);
            if (rc != EFS_OK)
                goto fail;
        }
    }
    coord = efs_txn_coordinator(&t, &p);
    rc = sim_txn_propose_decide(sim, coord, &t, EFS_TXN_COMMIT);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < p.n; i++) {
        rc = sim_txn_propose_resolve(sim, p.shard[i], &t, EFS_TXN_COMMIT);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
fail:
    for (i = 0; i < p.n; i++)
        (void)sim_txn_propose_drop(sim, p.shard[i], &t);
    return rc;
}

int efs_sim_dir_migrate(struct efs_sim *sim, efs_ino_t dir)
{
    struct efs_kv *kv, *hkv;
    char name[EFS_MAX_NAME];
    uint8_t hv[EFS_META_DENT_BYTES];
    uint8_t hk[EFS_KV_KEY_MAX];
    uint32_t hsh = 0, psh, hklen = 0, hvlen;
    uint8_t pg, hg;
    int rc, saw = 0;

    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    psh = efs_kv_inode_shard(dir);
    pg = sim_shard_group(psh);
    rc = sim_raft_read_group(sim, pg);
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, pg);
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_meta_dir_migrate_peek(kv, dir, name, sizeof(name), &hsh, &saw);
    if (rc == EFS_ERR_INVAL)
        return propose_dir(sim, EFS_MD_DIR_MIGRATE, dir);
    if (rc == EFS_ERR_NOT_FOUND) {
        struct efs_meta_row row;

        if (saw && efs_meta_apply_get_inode(kv, dir, &row) == EFS_OK &&
            (row.used_shards & 1ull) == 0)
            return propose_dir(sim, EFS_MD_DIR_MIGRATE, dir);
        return EFS_ERR_NOT_FOUND;
    }
    if (rc != EFS_OK)
        return rc;
    hg = sim_shard_group(hsh);
    if (hg == pg)
        return propose_dir(sim, EFS_MD_DIR_MIGRATE, dir);
    rc = sim_raft_read_group(sim, hg);
    if (rc != EFS_OK)
        return rc;
    hkv = sim_raft_kv_group(sim, hg);
    if (!hkv)
        return EFS_ERR_BUSY;
    rc = efs_kv_key_dentry(hsh, dir, name, hk, &hklen);
    if (rc != EFS_OK)
        return rc;
    hvlen = sizeof(hv);
    rc = efs_kv_get(hkv, hk, hklen, hv, &hvlen);
    if (rc == EFS_OK)
        return propose_dir(sim, EFS_MD_DIR_MIGRATE, dir);
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    return migrate_txn(sim, dir, name, hsh);
}

int efs_sim_dir_finish_hashed(struct efs_sim *sim, efs_ino_t dir)
{
    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    return propose_dir(sim, EFS_MD_DIR_FINISH, dir);
}

void sim_dir_maybe_drain(struct efs_sim *sim, efs_ino_t dir, uint8_t before)
{
    uint8_t after = 0;
    int rc, n;

    if (!sim || dir == 0)
        return;
    if (efs_sim_dir_layout(sim, dir, &after, NULL) != EFS_OK)
        return;
    if (after == EFS_META_LAYOUT_SPLITTING)
        efs_dir_spread_note(dir);
    if (before != EFS_META_LAYOUT_LOCAL || after != EFS_META_LAYOUT_SPLITTING)
        return;
    for (n = 0; n < 4096; n++) {
        rc = efs_sim_dir_migrate(sim, dir);
        if (rc == EFS_OK)
            continue;
        if (rc == EFS_ERR_NOT_FOUND) {
            (void)efs_sim_dir_finish_hashed(sim, dir);
            return;
        }
        return;
    }
}

int efs_sim_dir_layout(struct efs_sim *sim, efs_ino_t dir, uint8_t *layout,
                       uint64_t *epoch)
{
    struct efs_meta_row row;
    struct efs_kv *kv;
    uint32_t shard;
    int rc;

    if (!sim || dir == 0)
        return EFS_ERR_INVAL;
    shard = efs_kv_inode_shard(dir);
    rc = sim_raft_read_group(sim, sim_shard_group(shard));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (layout)
        *layout = row.layout;
    if (epoch)
        *epoch = row.layout_epoch;
    return EFS_OK;
}
