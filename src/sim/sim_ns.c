/* Cross-shard namespace txns: LINK, UNLINK (when shards differ), RMDIR,
 * file and directory RENAME. MKDIR stays in sim_txn.c. */
#include "sim_internal.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/dir_layout.h"
#include <string.h>
#include <sys/stat.h>

#define NS_PREP_MAX 16

struct ns_prep {
    uint32_t shard;
    int kind;
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen;
    uint64_t expected;
    int op;
    uint8_t val[EFS_META_INO_BYTES];
    uint32_t vlen;
    struct efs_txn_reduce red;
};

static void wr64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static int prep_add(struct ns_prep *pr, int *n, uint32_t shard, int kind,
                    const uint8_t *key, uint32_t klen, uint64_t expected, int op,
                    const uint8_t *val, uint32_t vlen,
                    const struct efs_txn_reduce *red)
{
    struct ns_prep *e;

    if (*n >= NS_PREP_MAX || klen > EFS_KV_KEY_MAX || vlen > EFS_META_INO_BYTES)
        return EFS_ERR_INVAL;
    e = &pr[*n];
    memset(e, 0, sizeof(*e));
    e->shard = shard;
    e->kind = kind;
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->expected = expected;
    e->op = op;
    if (val && vlen) {
        memcpy(e->val, val, vlen);
        e->vlen = vlen;
    }
    if (red)
        e->red = *red;
    (*n)++;
    return EFS_OK;
}

/* The dead inode's REAP marker (L7) as a PREP on its inode group's anchor
 * shard — the sim mirror of the marker the raft host adds to last-link
 * unlink/rename txns (host_unlink_txn / server_raft_host_rename_at).
 * `kv` must hold the anchor shard's group (same group as `ish`). */
static int reap_prep_add(struct efs_kv *kv, struct ns_prep *pr, int *n,
                         struct efs_txn_parts *p, uint32_t ish,
                         const struct efs_meta_row *row)
{
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint32_t krl = 0, ash;
    uint64_t rver = 0;
    int rc;

    ash = efs_kv_anchor_shard(ish);
    rc = efs_kv_key_reap(ash, row->ino, k_reap, &krl);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(kv, k_reap, krl, &rver);
    if (rc == EFS_OK)
        efs_meta_pack_reap(v_reap, row->generation, row->active_lanes);
    if (rc == EFS_OK)
        rc = sim_txn_parts_add(p, ash);
    if (rc == EFS_OK)
        rc = prep_add(pr, n, ash, EFS_TXN_EXCL, k_reap, krl, rver,
                      EFS_TXN_PUT, v_reap, sizeof(v_reap), NULL);
    return rc;
}

static int prep_issue(struct efs_sim *sim, const struct efs_txid *t,
                      const struct efs_txn_parts *p, struct ns_prep *pr, int n)
{
    int i, j, rc;

    for (i = 0; i < p->n; i++) {
        for (j = 0; j < n; j++) {
            if (pr[j].shard != p->shard[i])
                continue;
            rc = sim_txn_propose_prep(sim, pr[j].shard, pr[j].kind, t, p,
                                      pr[j].key, pr[j].klen, pr[j].expected,
                                      pr[j].op, pr[j].val, pr[j].vlen,
                                      pr[j].kind == EFS_TXN_REDUCE ? &pr[j].red
                                                                   : NULL);
            if (rc != EFS_OK)
                return rc;
        }
    }
    return EFS_OK;
}

static void prep_drop(struct efs_sim *sim, const struct efs_txid *t,
                      const struct efs_txn_parts *p)
{
    int i;

    for (i = 0; i < p->n; i++)
        (void)sim_txn_propose_drop(sim, p->shard[i], t);
}

static int load_row(struct efs_sim *sim, efs_ino_t ino, struct efs_meta_row *row)
{
    struct efs_kv *kv;
    int rc;

    rc = sim_txn_read_kv(sim, efs_kv_inode_shard(ino), &kv);
    if (rc != EFS_OK)
        return rc;
    return efs_meta_apply_get_inode(kv, ino, row);
}

static int dseq_prep(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                     uint8_t lane, struct ns_prep *pr, int *n,
                     struct efs_txn_parts *p)
{
    uint8_t k[EFS_KV_KEY_MAX], v[8], buf[8];
    uint32_t kl = 0, vn = 8;
    uint64_t ver = 0, seq = 0;
    int rc, gr;

    rc = efs_kv_key_dseq(shard, dir, lane, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(kv, k, kl, &ver);
    if (rc != EFS_OK)
        return rc;
    gr = efs_kv_get(kv, k, kl, buf, &vn);
    if (gr == EFS_OK && vn >= 8)
        seq = rd64(buf);
    else if (gr != EFS_OK && gr != EFS_ERR_NOT_FOUND)
        return gr;
    wr64(v, seq + 1);
    rc = sim_txn_parts_add(p, shard);
    if (rc != EFS_OK)
        return rc;
    return prep_add(pr, n, shard, EFS_TXN_EXCL, k, kl, ver, EFS_TXN_PUT, v, 8,
                    NULL);
}

static int dseq_guard(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                      uint8_t lane, struct ns_prep *pr, int *n,
                      struct efs_txn_parts *p)
{
    uint8_t k[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    uint64_t ver = 0;
    int rc;

    rc = efs_kv_key_dseq(shard, dir, lane, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(kv, k, kl, &ver);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_parts_add(p, shard);
    if (rc != EFS_OK)
        return rc;
    return prep_add(pr, n, shard, EFS_TXN_GUARD, k, kl, ver, 0, NULL, 0, NULL);
}

static int pver_guard_chain(struct efs_sim *sim, efs_ino_t dst_parent,
                            efs_ino_t src, struct ns_prep *pr, int *n,
                            struct efs_txn_parts *p)
{
    efs_ino_t cur = dst_parent;
    int hops, rc;

    for (hops = 0; hops < 64; hops++) {
        struct efs_meta_row r;
        struct efs_kv *kv;
        uint8_t k[EFS_KV_KEY_MAX];
        uint32_t kl = 0, sh;
        uint64_t ver = 0;

        if (cur == src)
            return EFS_ERR_INVAL;
        rc = load_row(sim, cur, &r);
        if (rc != EFS_OK)
            return rc;
        sh = efs_kv_inode_shard(cur);
        rc = sim_txn_read_kv(sim, sh, &kv);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_key_pver(sh, cur, k, &kl);
        if (rc == EFS_OK)
            rc = efs_txn_ver_get(kv, k, kl, &ver);
        if (rc == EFS_OK)
            rc = sim_txn_parts_add(p, sh);
        if (rc == EFS_OK)
            rc = prep_add(pr, n, sh, EFS_TXN_GUARD, k, kl, ver, 0, NULL, 0,
                          NULL);
        if (rc != EFS_OK)
            return rc;
        if (cur == EFS_ROOT_INO || cur == r.parent)
            return EFS_OK;
        cur = r.parent;
    }
    return EFS_ERR_INVAL;
}

static int pver_bump(struct efs_sim *sim, efs_ino_t ino, uint64_t new_ver,
                     struct ns_prep *pr, int *n, struct efs_txn_parts *p)
{
    uint8_t k[EFS_KV_KEY_MAX], v[8];
    uint32_t kl = 0, sh = efs_kv_inode_shard(ino);
    uint64_t ever = 0;
    struct efs_kv *kv;
    int rc = sim_txn_read_kv(sim, sh, &kv);

    if (rc == EFS_OK)
        rc = efs_kv_key_pver(sh, ino, k, &kl);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(kv, k, kl, &ever);
    if (rc == EFS_OK)
        rc = sim_txn_parts_add(p, sh);
    if (rc != EFS_OK)
        return rc;
    wr64(v, new_ver);
    return prep_add(pr, n, sh, EFS_TXN_EXCL, k, kl, ever, EFS_TXN_PUT, v, 8,
                    NULL);
}

static int stamp_local_parent(struct efs_sim *sim, struct efs_meta_row *prow,
                              efs_ino_t parent, uint64_t now, struct ns_prep *pr,
                              int *n, struct efs_txn_parts *p, int nents_delta)
{
    uint8_t k[EFS_KV_KEY_MAX], v[EFS_META_INO_BYTES];
    uint32_t kl = 0;
    uint64_t ver = 0;
    uint32_t psh = efs_kv_inode_shard(parent);
    struct efs_kv *kv;
    int rc;

    rc = sim_txn_read_kv(sim, psh, &kv);
    if (rc != EFS_OK)
        return rc;
    if (prow->base_mtime < now)
        prow->base_mtime = now;
    if (prow->base_ctime < now)
        prow->base_ctime = now;
    efs_meta_dir_note_entry(prow, nents_delta);
    rc = efs_meta_pack_inode(prow, v, sizeof(v));
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_inode(psh, parent, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(kv, k, kl, &ver);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_parts_add(p, psh);
    if (rc != EFS_OK)
        return rc;
    return prep_add(pr, n, psh, EFS_TXN_EXCL, k, kl, ver, EFS_TXN_PUT, v,
                    EFS_META_INO_BYTES, NULL);
}

static int stamp_hashed_lane(struct efs_kv *kv, const struct efs_meta_row *dir,
                             const char *name, uint64_t now, struct ns_prep *pr,
                             int *n, struct efs_txn_parts *p)
{
    uint8_t lane = efs_kv_dir_lane(name);
    uint32_t lsh = efs_kv_lane_shard(dir->ino, lane);
    uint8_t k[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    struct efs_txn_reduce red;
    int rc;

    (void)kv;
    rc = efs_kv_key_lane(lsh, dir->ino, dir->generation, lane, k, &kl);
    if (rc != EFS_OK)
        return rc;
    memset(&red, 0, sizeof(red));
    red.max_mtime = now;
    red.max_ctime = now;
    rc = sim_txn_parts_add(p, lsh);
    if (rc != EFS_OK)
        return rc;
    return prep_add(pr, n, lsh, EFS_TXN_REDUCE, k, kl, 0, 0, NULL, 0, &red);
}

static int drop_dentry_prep(struct efs_sim *sim, const struct efs_meta_row *prow,
                            efs_ino_t parent, const char *name,
                            struct ns_prep *pr, int *n, struct efs_txn_parts *p)
{
    uint32_t psh = efs_kv_inode_shard(parent);
    uint32_t hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], v_tomb[EFS_META_DENT_BYTES];
    uint32_t kl = 0, kh = 0;
    uint64_t ver = 0;
    struct efs_kv *kv;
    struct efs_meta_dentry tomb;
    int rc;

    rc = efs_kv_key_dentry(psh, parent, name, k_loc, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, &kh);
    if (rc != EFS_OK)
        return rc;
    if (prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING && kl == kh &&
          memcmp(k_loc, k_hash, kl) == 0)) {
        rc = sim_txn_read_kv(sim, psh, &kv);
        if (rc != EFS_OK)
            return rc;
        rc = efs_txn_ver_get(kv, k_loc, kl, &ver);
        if (rc != EFS_OK)
            return rc;
        rc = sim_txn_parts_add(p, psh);
        if (rc != EFS_OK)
            return rc;
        rc = prep_add(pr, n, psh, EFS_TXN_EXCL, k_loc, kl, ver, EFS_TXN_DEL,
                      NULL, 0, NULL);
        if (rc != EFS_OK)
            return rc;
    }
    if (prow->layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow->layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        rc = efs_meta_pack_dentry(&tomb, v_tomb, sizeof(v_tomb));
        if (rc != EFS_OK)
            return rc;
        rc = sim_txn_read_kv(sim, hsh, &kv);
        if (rc != EFS_OK)
            return rc;
        rc = efs_txn_ver_get(kv, k_hash, kh, &ver);
        if (rc != EFS_OK)
            return rc;
        rc = sim_txn_parts_add(p, hsh);
        if (rc != EFS_OK)
            return rc;
        return prep_add(pr, n, hsh, EFS_TXN_EXCL, k_hash, kh, ver, EFS_TXN_PUT,
                        v_tomb, EFS_META_DENT_BYTES, NULL);
    }
    if (prow->layout == EFS_META_LAYOUT_HASHED) {
        rc = sim_txn_read_kv(sim, hsh, &kv);
        if (rc != EFS_OK)
            return rc;
        rc = efs_txn_ver_get(kv, k_hash, kh, &ver);
        if (rc != EFS_OK)
            return rc;
        rc = sim_txn_parts_add(p, hsh);
        if (rc != EFS_OK)
            return rc;
        return prep_add(pr, n, hsh, EFS_TXN_EXCL, k_hash, kh, ver, EFS_TXN_DEL,
                        NULL, 0, NULL);
    }
    return EFS_OK;
}

static int finish_build(struct efs_sim *sim, int client, struct efs_txid *t,
                        struct efs_txn_parts *p, struct ns_prep *pr, int n)
{
    int rc, i;

    if (client < 0 || client >= sim->nclients)
        client = 0;
    for (i = 0; i < p->n; i++) {
        rc = sim_sess_ensure(sim, client, p->shard[i]);
        if (rc != EFS_OK)
            return rc;
    }
    sim_txn_fill_txid(sim, t);
    rc = prep_issue(sim, t, p, pr, n);
    if (rc != EFS_OK) {
        prep_drop(sim, t, p);
        return rc;
    }
    sim->txn_id = *t;
    sim->txn_parts = *p;
    sim->txn_live = 1;
    return EFS_OK;
}

static int link_build(struct efs_sim *sim, int client, efs_ino_t src_parent,
                      const char *src_name, efs_ino_t dst_parent,
                      const char *dst_name, struct efs_txid *t,
                      struct efs_txn_parts *p)
{
    struct efs_meta_dentry src, ndent, dst;
    struct efs_meta_row row, dprow;
    struct efs_kv *dkv, *ikv;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_ino[EFS_META_INO_BYTES];
    uint32_t kd = 0, ki = 0, dsh, ish;
    uint64_t iver = 0, dver = 0;
    struct ns_prep pr[NS_PREP_MAX];
    int n = 0, rc;
    uint8_t dseq_lane;

    memset(p, 0, sizeof(*p));
    rc = sim_txn_lookup(sim, src_parent, src_name, &src);
    if (rc != EFS_OK)
        return rc;
    rc = load_row(sim, src.ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    rc = load_row(sim, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = sim_txn_lookup(sim, dst_parent, dst_name, &dst);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    ish = efs_kv_inode_shard(row.ino);
    dseq_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0
                                                     : efs_kv_dir_lane(dst_name);
    rc = sim_txn_read_kv(sim, dsh, &dkv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(dkv, k_dent, kd, &dver);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_read_kv(sim, ish, &ikv);
    if (rc != EFS_OK)
        return rc;

    memset(&ndent, 0, sizeof(ndent));
    ndent.ino = row.ino;
    ndent.generation = row.generation;
    ndent.type = row.mode & S_IFMT;
    row.nlink++;
    if (row.base_ctime < sim->now)
        row.base_ctime = sim->now;
    rc = efs_meta_pack_dentry(&ndent, v_dent, sizeof(v_dent));
    if (rc == EFS_OK)
        rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(ikv, k_ino, ki, &iver);
    if (rc != EFS_OK)
        return rc;

    rc = sim_txn_parts_add(p, dsh);
    if (rc == EFS_OK)
        rc = sim_txn_parts_add(p, ish);
    if (rc != EFS_OK)
        return rc;
    rc = prep_add(pr, &n, dsh, EFS_TXN_EXCL, k_dent, kd, dver, EFS_TXN_PUT, v_dent,
                  EFS_META_DENT_BYTES, NULL);
    if (rc == EFS_OK)
        rc = prep_add(pr, &n, ish, EFS_TXN_EXCL, k_ino, ki, iver, EFS_TXN_PUT,
                      v_ino, EFS_META_INO_BYTES, NULL);
    if (rc != EFS_OK)
        return rc;
    if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
        rc = stamp_local_parent(sim, &dprow, dst_parent, sim->now, pr, &n, p, 1);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = stamp_hashed_lane(dkv, &dprow, dst_name, sim->now, pr, &n, p);
        if (rc != EFS_OK)
            return rc;
        if ((dprow.used_shards & (1ull << efs_kv_dir_lane(dst_name))) == 0) {
            dprow.used_shards |= 1ull << efs_kv_dir_lane(dst_name);
            rc = stamp_local_parent(sim, &dprow, dst_parent, sim->now, pr, &n,
                                    p, 0);
            if (rc != EFS_OK)
                return rc;
        }
    }
    rc = dseq_prep(dkv, dsh, dst_parent, dseq_lane, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    return finish_build(sim, client, t, p, pr, n);
}

static int unlink_build(struct efs_sim *sim, int client, efs_ino_t parent, const char *name,
                        struct efs_txid *t, struct efs_txn_parts *p)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, prow;
    struct efs_kv *pkv, *ikv;
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[EFS_META_INO_BYTES];
    uint32_t ki = 0, dsh, ish, psh;
    uint64_t iver = 0;
    struct ns_prep pr[NS_PREP_MAX];
    int n = 0, rc, held;
    uint8_t dseq_lane;

    memset(p, 0, sizeof(*p));
    rc = sim_txn_lookup(sim, parent, name, &dent);
    if (rc != EFS_OK)
        return rc;
    rc = load_row(sim, dent.ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    rc = load_row(sim, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    psh = efs_kv_inode_shard(parent);
    dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    ish = efs_kv_inode_shard(row.ino);
    rc = sim_txn_read_kv(sim, dsh, &pkv);
    if (rc != EFS_OK)
        return rc;
    rc = drop_dentry_prep(sim, &prow, parent, name, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_read_kv(sim, ish, &ikv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(ikv, k_ino, ki, &iver);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_parts_add(p, ish);
    if (rc != EFS_OK)
        return rc;
    if (row.nlink <= 1) {
        held = efs_lease_any(ikv, row.ino, row.generation);
        if (held < 0)
            return held;
        if (held) {
            row.nlink = 0;
            if (row.base_ctime < sim->now)
                row.base_ctime = sim->now;
            rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
            if (rc != EFS_OK)
                return rc;
            rc = prep_add(pr, &n, ish, EFS_TXN_EXCL, k_ino, ki, iver,
                          EFS_TXN_PUT, v_ino, EFS_META_INO_BYTES, NULL);
        } else {
            rc = prep_add(pr, &n, ish, EFS_TXN_EXCL, k_ino, ki, iver,
                          EFS_TXN_DEL, NULL, 0, NULL);
            /* Last link, no lease: the reap marker (L7) rides the same
             * txn, mirroring host_unlink_txn. */
            if (rc == EFS_OK)
                rc = reap_prep_add(ikv, pr, &n, p, ish, &row);
        }
    } else {
        row.nlink--;
        if (row.base_ctime < sim->now)
            row.base_ctime = sim->now;
        rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
        if (rc != EFS_OK)
            return rc;
        rc = prep_add(pr, &n, ish, EFS_TXN_EXCL, k_ino, ki, iver, EFS_TXN_PUT,
                      v_ino, EFS_META_INO_BYTES, NULL);
    }
    if (rc != EFS_OK)
        return rc;
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        rc = sim_txn_read_kv(sim, psh, &pkv);
        if (rc != EFS_OK)
            return rc;
        rc = stamp_local_parent(sim, &prow, parent, sim->now, pr, &n, p, -1);
    } else {
        rc = stamp_hashed_lane(pkv, &prow, name, sim->now, pr, &n, p);
    }
    if (rc != EFS_OK)
        return rc;
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    rc = dseq_prep(pkv, dsh, parent, dseq_lane, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    return finish_build(sim, client, t, p, pr, n);
}

static int dir_scan_live_cb(void *user, const uint8_t *key, uint32_t klen,
                            const uint8_t *val, uint32_t vlen)
{
    struct efs_meta_dentry d;
    int *live = user;

    (void)key;
    if (klen <= 11)
        return 0;
    if (efs_meta_unpack_dentry(val, vlen, &d) != EFS_OK)
        return 0;
    if (d.type == EFS_META_DENT_TOMBSTONE)
        return 0;
    *live = 1;
    return 1;
}

static int shard_empty(struct efs_kv *kv, uint32_t shard, efs_ino_t dir)
{
    uint8_t pre[EFS_KV_KEY_MAX];
    uint32_t pl = 0;
    int live = 0, rc;

    rc = efs_kv_key_dentry_prefix(shard, dir, pre, &pl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan_prefix(kv, pre, pl, dir_scan_live_cb, &live);
    if (rc != EFS_OK && rc != 1)
        return rc;
    return live ? EFS_ERR_NOT_EMPTY : EFS_OK;
}

static int rmdir_build(struct efs_sim *sim, int client, efs_ino_t parent, const char *name,
                       struct efs_txid *t, struct efs_txn_parts *p)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, prow;
    struct efs_kv *dkv, *ckv, *pkv;
    uint8_t k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t v_par[EFS_META_INO_BYTES];
    uint32_t ki = 0, kp = 0, csh, psh, dsh;
    uint64_t cver = 0, pver = 0;
    struct ns_prep pr[NS_PREP_MAX];
    int n = 0, rc, held;
    uint8_t lane, dseq_lane;

    memset(p, 0, sizeof(*p));
    rc = sim_txn_lookup(sim, parent, name, &dent);
    if (rc != EFS_OK)
        return rc;
    rc = load_row(sim, dent.ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(row.mode) || row.ino == EFS_ROOT_INO)
        return EFS_ERR_INVAL;
    if (row.layout == EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_BUSY;
    if (row.nlink > 2)
        return EFS_ERR_NOT_EMPTY;
    csh = efs_kv_inode_shard(row.ino);
    rc = sim_txn_read_kv(sim, csh, &ckv);
    if (rc != EFS_OK)
        return rc;
    if (row.layout == EFS_META_LAYOUT_LOCAL) {
        rc = shard_empty(ckv, csh, row.ino);
        if (rc != EFS_OK)
            return rc;
    } else {
        for (lane = 0; lane < EFS_META_LANES; lane++) {
            uint32_t lsh;

            if ((row.used_shards & (1ull << lane)) == 0)
                continue;
            lsh = efs_kv_lane_shard(row.ino, lane);
            rc = sim_txn_read_kv(sim, lsh, &dkv);
            if (rc != EFS_OK)
                return rc;
            rc = shard_empty(dkv, lsh, row.ino);
            if (rc != EFS_OK)
                return rc;
            rc = dseq_guard(dkv, lsh, row.ino, lane, pr, &n, p);
            if (rc != EFS_OK)
                return rc;
        }
    }
    held = efs_lease_any(ckv, row.ino, row.generation);
    if (held < 0)
        return held;
    if (held)
        return EFS_ERR_BUSY;
    rc = load_row(sim, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    if (prow.nlink < 3)
        return EFS_ERR_PROTO;
    psh = efs_kv_inode_shard(parent);
    dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    rc = sim_txn_read_kv(sim, dsh, &pkv);
    if (rc != EFS_OK)
        return rc;
    prow.nlink--;
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        if (prow.base_mtime < sim->now)
            prow.base_mtime = sim->now;
        if (prow.base_ctime < sim->now)
            prow.base_ctime = sim->now;
        efs_meta_dir_note_entry(&prow, -1);
    }
    rc = efs_meta_pack_inode(&prow, v_par, sizeof(v_par));
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_read_kv(sim, psh, &dkv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(dkv, k_par, kp, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(ckv, k_ino, ki, &cver);
    if (rc != EFS_OK)
        return rc;
    rc = drop_dentry_prep(sim, &prow, parent, name, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_parts_add(p, psh);
    if (rc == EFS_OK)
        rc = sim_txn_parts_add(p, csh);
    if (rc != EFS_OK)
        return rc;
    rc = prep_add(pr, &n, psh, EFS_TXN_EXCL, k_par, kp, pver, EFS_TXN_PUT, v_par,
                  EFS_META_INO_BYTES, NULL);
    if (rc == EFS_OK)
        rc = prep_add(pr, &n, csh, EFS_TXN_EXCL, k_ino, ki, cver, EFS_TXN_DEL,
                      NULL, 0, NULL);
    if (rc != EFS_OK)
        return rc;
    if (prow.layout != EFS_META_LAYOUT_LOCAL) {
        rc = stamp_hashed_lane(pkv, &prow, name, sim->now, pr, &n, p);
        if (rc != EFS_OK)
            return rc;
    } else {
        uint8_t k_dseq[EFS_KV_KEY_MAX];
        uint32_t ks = 0;

        rc = efs_kv_key_dseq(csh, row.ino, 0, k_dseq, &ks);
        if (rc != EFS_OK)
            return rc;
        rc = efs_txn_ver_get(ckv, k_dseq, ks, &cver);
        if (rc != EFS_OK)
            return rc;
        rc = prep_add(pr, &n, csh, EFS_TXN_GUARD, k_dseq, ks, cver, 0, NULL, 0,
                      NULL);
        if (rc != EFS_OK)
            return rc;
    }
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    rc = dseq_prep(pkv, dsh, parent, dseq_lane, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    return finish_build(sim, client, t, p, pr, n);
}

static int rename_build(struct efs_sim *sim, int client, efs_ino_t src_parent,
                        const char *src_name, efs_ino_t dst_parent,
                        const char *dst_name, struct efs_txid *t,
                        struct efs_txn_parts *p)
{
    struct efs_meta_dentry src, exist;
    struct efs_meta_row row, sprow, dprow, nrow;
    struct efs_kv *skv, *dkv, *ikv, *nkv;
    uint8_t k_dent[EFS_KV_KEY_MAX], v_dent[EFS_META_DENT_BYTES];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[EFS_META_INO_BYTES];
    uint8_t k_nino[EFS_KV_KEY_MAX], v_nino[EFS_META_INO_BYTES];
    uint32_t kd = 0, ki = 0, dsh, ish, ssh, nsh = 0, kn = 0;
    uint64_t iver = 0, dver = 0, nver = 0;
    struct ns_prep pr[NS_PREP_MAX];
    int n = 0, rc, is_dir;
    int xist = 0, xdir = 0;
    uint8_t s_lane, d_lane;

    memset(p, 0, sizeof(*p));
    if (src_parent == dst_parent && strcmp(src_name, dst_name) == 0)
        return EFS_OK;
    rc = sim_txn_lookup(sim, src_parent, src_name, &src);
    if (rc != EFS_OK)
        return rc;
    rc = load_row(sim, src.ino, &row);
    if (rc != EFS_OK)
        return rc;
    is_dir = S_ISDIR(row.mode);
    if (is_dir && row.ino == EFS_ROOT_INO)
        return EFS_ERR_INVAL;
    rc = load_row(sim, src_parent, &sprow);
    if (rc != EFS_OK)
        return rc;
    rc = load_row(sim, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    if (is_dir) {
        rc = pver_guard_chain(sim, dst_parent, row.ino, pr, &n, p);
        if (rc != EFS_OK)
            return rc;
        row.parent_version++;
        if (src_parent != dst_parent) {
            if (sprow.nlink < 3)
                return EFS_ERR_PROTO;
            sprow.nlink--;
            dprow.nlink++;
        }
    }
    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    ssh = efs_kv_dentry_shard(src_parent, src_name, sprow.layout);
    ish = efs_kv_inode_shard(row.ino);
    s_lane = sprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(src_name);
    d_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(dst_name);
    rc = sim_txn_read_kv(sim, dsh, &dkv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc != EFS_OK)
        return rc;
    /* Rename-over-existing: hashed-then-local lookup (tombstone =
     * NOT_FOUND). Leftover dest is POSIX replace, not absent. The PUT
     * at k_dent overwrites the hashed dest; leftover local dest is
     * DELed below. Same (ino,gen) = hardlink self-rename: no dest. */
    {
        struct efs_meta_dentry xdent;
        int xrc = sim_txn_lookup(sim, dst_parent, dst_name, &xdent);
        if (xrc == EFS_OK) {
            int held;
            if (xdent.ino != row.ino || xdent.generation != row.generation) {
                xist = 1;
                xdir = S_ISDIR(xdent.type & S_IFMT) ? 1 : 0;
                if (is_dir != xdir)
                    return EFS_ERR_INVAL;
                nsh = efs_kv_inode_shard(xdent.ino);
                rc = load_row(sim, xdent.ino, &nrow);
                if (rc != EFS_OK)
                    return rc;
                rc = sim_txn_read_kv(sim, nsh, &nkv);
                if (rc != EFS_OK)
                    return rc;
                rc = efs_kv_key_inode(nsh, nrow.ino, k_nino, &kn);
                if (rc != EFS_OK)
                    return rc;
                rc = efs_txn_ver_get(nkv, k_nino, kn, &nver);
                if (rc != EFS_OK)
                    return rc;
                rc = sim_txn_parts_add(p, nsh);
                if (rc != EFS_OK)
                    return rc;
                if (xdir) {
                    uint8_t lane;

                    if (nrow.layout == EFS_META_LAYOUT_SPLITTING)
                        return EFS_ERR_BUSY;
                    if (nrow.layout != EFS_META_LAYOUT_LOCAL &&
                        nrow.layout != EFS_META_LAYOUT_HASHED)
                        return EFS_ERR_INVAL;
                    if (nrow.nlink > 2)
                        return EFS_ERR_NOT_EMPTY;
                    if (nrow.layout == EFS_META_LAYOUT_HASHED) {
                        for (lane = 0; lane < EFS_META_LANES; lane++) {
                            uint32_t lsh;
                            struct efs_kv *lkv;

                            if ((nrow.used_shards & (1ull << lane)) == 0)
                                continue;
                            lsh = efs_kv_lane_shard(nrow.ino, lane);
                            rc = sim_txn_read_kv(sim, lsh, &lkv);
                            if (rc != EFS_OK)
                                return rc;
                            rc = shard_empty(lkv, lsh, nrow.ino);
                            if (rc != EFS_OK)
                                return rc;
                            rc = dseq_guard(lkv, lsh, nrow.ino, lane, pr, &n, p);
                            if (rc != EFS_OK)
                                return rc;
                        }
                    } else {
                        rc = shard_empty(nkv, nsh, nrow.ino);
                        if (rc != EFS_OK)
                            return rc;
                    }
                    held = efs_lease_any(nkv, nrow.ino, nrow.generation);
                    if (held < 0)
                        return held;
                    if (held)
                        return EFS_ERR_BUSY;
                    /* the replaced subdir's parent loses one link */
                    if (src_parent == dst_parent) {
                        if (sprow.nlink < 3)
                            return EFS_ERR_PROTO;
                        sprow.nlink--;
                    } else {
                        if (dprow.nlink < 3)
                            return EFS_ERR_PROTO;
                        dprow.nlink--;
                    }
                    rc = prep_add(pr, &n, nsh, EFS_TXN_EXCL, k_nino, kn, nver,
                                  EFS_TXN_DEL, NULL, 0, NULL);
                    if (rc == EFS_OK && nrow.layout == EFS_META_LAYOUT_LOCAL)
                        rc = dseq_guard(nkv, nsh, nrow.ino, 0, pr, &n, p);
                    if (rc != EFS_OK)
                        return rc;
                } else if (nrow.nlink <= 1) {
                    held = efs_lease_any(nkv, nrow.ino, nrow.generation);
                    if (held < 0)
                        return held;
                    if (held) {
                        nrow.nlink = 0;
                        if (nrow.base_ctime < sim->now)
                            nrow.base_ctime = sim->now;
                        rc = efs_meta_pack_inode(&nrow, v_nino,
                                                 sizeof(v_nino));
                        if (rc != EFS_OK)
                            return rc;
                        rc = prep_add(pr, &n, nsh, EFS_TXN_EXCL, k_nino, kn,
                                      nver, EFS_TXN_PUT, v_nino,
                                      EFS_META_INO_BYTES, NULL);
                    } else {
                        rc = prep_add(pr, &n, nsh, EFS_TXN_EXCL, k_nino, kn,
                                      nver, EFS_TXN_DEL, NULL, 0, NULL);
                        /* Dest file retired at its last link: reap marker
                         * (L7) in the same txn, mirroring the raft host. */
                        if (rc == EFS_OK)
                            rc = reap_prep_add(nkv, pr, &n, p, nsh, &nrow);
                    }
                    if (rc != EFS_OK)
                        return rc;
                } else {
                    nrow.nlink--;
                    if (nrow.base_ctime < sim->now)
                        nrow.base_ctime = sim->now;
                    rc = efs_meta_pack_inode(&nrow, v_nino, sizeof(v_nino));
                    if (rc != EFS_OK)
                        return rc;
                    rc = prep_add(pr, &n, nsh, EFS_TXN_EXCL, k_nino, kn, nver,
                                  EFS_TXN_PUT, v_nino, EFS_META_INO_BYTES,
                                  NULL);
                    if (rc != EFS_OK)
                        return rc;
                }
            }
        } else if (xrc != EFS_ERR_NOT_FOUND) {
            return xrc;
        }
    }
    rc = efs_txn_ver_get(dkv, k_dent, kd, &dver);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_read_kv(sim, ssh, &skv);
    if (rc != EFS_OK)
        return rc;
    rc = sim_txn_read_kv(sim, ish, &ikv);
    if (rc != EFS_OK)
        return rc;

    memset(&exist, 0, sizeof(exist));
    exist.ino = row.ino;
    exist.generation = row.generation;
    exist.type = row.mode & S_IFMT;
    row.parent = dst_parent;
    if (row.base_ctime < sim->now)
        row.base_ctime = sim->now;
    rc = efs_meta_pack_dentry(&exist, v_dent, sizeof(v_dent));
    if (rc == EFS_OK)
        rc = efs_meta_pack_inode(&row, v_ino, sizeof(v_ino));
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(ish, row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(ikv, k_ino, ki, &iver);
    if (rc != EFS_OK)
        return rc;

    rc = drop_dentry_prep(sim, &sprow, src_parent, src_name, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    /* SPLITTING dest overwrite: hashed PUT does not remove a leftover
     * local dest. DEL it unless the keys alias (lane 0). */
    if (xist && dprow.layout == EFS_META_LAYOUT_SPLITTING) {
        uint32_t dpsh = efs_kv_inode_shard(dst_parent);
        uint8_t k_dloc[EFS_KV_KEY_MAX];
        uint32_t kdl = 0;
        uint64_t locver = 0;
        struct efs_kv *lkv;

        rc = efs_kv_key_dentry(dpsh, dst_parent, dst_name, k_dloc, &kdl);
        if (rc != EFS_OK)
            return rc;
        if (!(kdl == kd && memcmp(k_dloc, k_dent, kdl) == 0)) {
            rc = sim_txn_read_kv(sim, dpsh, &lkv);
            if (rc != EFS_OK)
                return rc;
            rc = efs_txn_ver_get(lkv, k_dloc, kdl, &locver);
            if (rc != EFS_OK)
                return rc;
            rc = sim_txn_parts_add(p, dpsh);
            if (rc != EFS_OK)
                return rc;
            rc = prep_add(pr, &n, dpsh, EFS_TXN_EXCL, k_dloc, kdl, locver,
                          EFS_TXN_DEL, NULL, 0, NULL);
            if (rc != EFS_OK)
                return rc;
        }
    }
    rc = sim_txn_parts_add(p, dsh);
    if (rc == EFS_OK)
        rc = sim_txn_parts_add(p, ish);
    if (rc != EFS_OK)
        return rc;
    rc = prep_add(pr, &n, dsh, EFS_TXN_EXCL, k_dent, kd, dver, EFS_TXN_PUT, v_dent,
                  EFS_META_DENT_BYTES, NULL);
    if (rc == EFS_OK)
        rc = prep_add(pr, &n, ish, EFS_TXN_EXCL, k_ino, ki, iver, EFS_TXN_PUT,
                      v_ino, EFS_META_INO_BYTES, NULL);
    if (rc == EFS_OK && is_dir)
        rc = pver_bump(sim, row.ino, row.parent_version, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    if (sprow.layout == EFS_META_LAYOUT_LOCAL) {
        rc = stamp_local_parent(sim, &sprow, src_parent, sim->now, pr, &n, p,
                                (src_parent == dst_parent && !xist) ? 0 : -1);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = stamp_hashed_lane(skv, &sprow, src_name, sim->now, pr, &n, p);
        if (rc != EFS_OK)
            return rc;
        if (is_dir && src_parent != dst_parent) {
            rc = stamp_local_parent(sim, &sprow, src_parent, sim->now, pr, &n,
                                    p, 0);
            if (rc != EFS_OK)
                return rc;
        }
    }
    if (src_parent != dst_parent) {
        if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
            rc = stamp_local_parent(sim, &dprow, dst_parent, sim->now, pr, &n,
                                    p, xist ? 0 : 1);
        } else {
            rc = stamp_hashed_lane(dkv, &dprow, dst_name, sim->now, pr, &n, p);
            if (rc == EFS_OK && is_dir)
                rc = stamp_local_parent(sim, &dprow, dst_parent, sim->now, pr,
                                        &n, p, 0);
        }
        if (rc != EFS_OK)
            return rc;
    } else if (dprow.layout != EFS_META_LAYOUT_LOCAL) {
        rc = stamp_hashed_lane(dkv, &dprow, dst_name, sim->now, pr, &n, p);
        if (rc != EFS_OK)
            return rc;
    }
    rc = dseq_prep(skv, ssh, src_parent, s_lane, pr, &n, p);
    if (rc == EFS_OK && !(ssh == dsh && s_lane == d_lane))
        rc = dseq_prep(dkv, dsh, dst_parent, d_lane, pr, &n, p);
    if (rc != EFS_OK)
        return rc;
    return finish_build(sim, client, t, p, pr, n);
}

static int run_op(struct efs_sim *sim, int client, struct efs_txid *txid,
                  int until)
{
    int rc;

    if (txid)
        *txid = sim->txn_id;
    rc = sim_txn_run_until(sim, client, until);
    return rc;
}

int sim_txn_link_until(struct efs_sim *sim, int client, efs_ino_t src_parent,
                       const char *src_name, efs_ino_t dst_parent,
                       const char *dst_name, struct efs_txid *txid, int until)
{
    struct efs_txid t;
    struct efs_txn_parts p;
    int rc;

    if (!sim || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    rc = link_build(sim, client, src_parent, src_name, dst_parent, dst_name, &t, &p);
    if (rc != EFS_OK)
        return rc;
    return run_op(sim, client, txid, until);
}

int sim_txn_unlink_until(struct efs_sim *sim, int client, efs_ino_t parent,
                         const char *name, struct efs_txid *txid, int until)
{
    struct efs_txid t;
    struct efs_txn_parts p;
    int rc;

    if (!sim || !name || parent == 0)
        return EFS_ERR_INVAL;
    rc = unlink_build(sim, client, parent, name, &t, &p);
    if (rc != EFS_OK)
        return rc;
    return run_op(sim, client, txid, until);
}

int sim_txn_rmdir_until(struct efs_sim *sim, int client, efs_ino_t parent,
                        const char *name, struct efs_txid *txid, int until)
{
    struct efs_txid t;
    struct efs_txn_parts p;
    int rc;

    if (!sim || !name || parent == 0)
        return EFS_ERR_INVAL;
    rc = rmdir_build(sim, client, parent, name, &t, &p);
    if (rc != EFS_OK)
        return rc;
    return run_op(sim, client, txid, until);
}

int sim_txn_rename_until(struct efs_sim *sim, int client, efs_ino_t src_parent,
                         const char *src_name, efs_ino_t dst_parent,
                         const char *dst_name, struct efs_txid *txid, int until)
{
    struct efs_txid t;
    struct efs_txn_parts p;
    int rc;

    if (!sim || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    if (src_parent == dst_parent && strcmp(src_name, dst_name) == 0)
        return EFS_OK;
    rc = rename_build(sim, client, src_parent, src_name, dst_parent, dst_name, &t, &p);
    if (rc != EFS_OK)
        return rc;
    return run_op(sim, client, txid, until);
}

int efs_sim_link(struct efs_sim *sim, int client, efs_ino_t src_parent,
                 const char *src_name, efs_ino_t dst_parent, const char *dst_name)
{
    return sim_txn_link_until(sim, client, src_parent, src_name, dst_parent,
                              dst_name, NULL, EFS_SIM_TXN_RESOLVE);
}

int efs_sim_link_until(struct efs_sim *sim, efs_ino_t src_parent,
                       const char *src_name, efs_ino_t dst_parent,
                       const char *dst_name, struct efs_txid *txid, int until)
{
    return sim_txn_link_until(sim, 0, src_parent, src_name, dst_parent, dst_name,
                              txid, until);
}

int efs_sim_rmdir(struct efs_sim *sim, int client, efs_ino_t parent,
                  const char *name)
{
    return sim_txn_rmdir_until(sim, client, parent, name, NULL,
                               EFS_SIM_TXN_RESOLVE);
}

int efs_sim_rmdir_until(struct efs_sim *sim, efs_ino_t parent, const char *name,
                        struct efs_txid *txid, int until)
{
    return sim_txn_rmdir_until(sim, 0, parent, name, txid, until);
}

int efs_sim_rename(struct efs_sim *sim, int client, efs_ino_t src_parent,
                   const char *src_name, efs_ino_t dst_parent,
                   const char *dst_name)
{
    return sim_txn_rename_until(sim, client, src_parent, src_name, dst_parent,
                                dst_name, NULL, EFS_SIM_TXN_RESOLVE);
}

int efs_sim_rename_until(struct efs_sim *sim, efs_ino_t src_parent,
                         const char *src_name, efs_ino_t dst_parent,
                         const char *dst_name, struct efs_txid *txid, int until)
{
    return sim_txn_rename_until(sim, 0, src_parent, src_name, dst_parent,
                                dst_name, txid, until);
}
