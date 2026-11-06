/* meta_apply/namespace.c: UNLINK, expiry, LINK, RENAME, SETATTR. */

#include "apply_internal.h"
#include "namespace.h"
#include "rows.h"
#include "init.h"
#include "xattr.h"

int efs_meta_apply_unlink(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          uint64_t now)
{
    return efs_meta_apply_unlink_op(kv, parent, name, now, NULL);
}

/* Recover a legacy dangling regular-file name only inside one authoritative
 * LOCAL shard. Never synthesize an inode, touch object/reap state, or infer
 * absence from another shard. Apply serialization plus intent guards make
 * absence and the name removal one atomic state-machine operation. */
static int apply_orphan_unlink(struct efs_kv *kv, efs_ino_t parent,
                               const char *name, uint64_t now,
                               const struct efs_opid_req *q)
{
    struct efs_meta_row prow, row;
    struct efs_meta_dentry dent;
    uint8_t kd[EFS_KV_KEY_MAX], ki[EFS_KV_KEY_MAX], kp[EFS_KV_KEY_MAX];
    uint8_t ks[EFS_KV_KEY_MAX], ko[EFS_KV_KEY_MAX];
    uint8_t vp[INO_VAL], vs[8], vo[EFS_OPID_VAL_MAX];
    uint32_t nd, ni, np, ns, sh;
    int rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK) return rc;
    if (!S_ISDIR(prow.mode) || prow.layout != EFS_META_LAYOUT_LOCAL)
        return EFS_ERR_NOT_FOUND;
    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc != EFS_OK) return rc;
    sh = efs_kv_inode_shard(parent);
    if ((dent.type & S_IFMT) != S_IFREG || !dent.ino ||
        efs_kv_inode_shard(dent.ino) != sh)
        return EFS_ERR_NOT_FOUND;
    rc = efs_kv_key_dentry(sh,parent,name,kd,&nd);
    if (rc == EFS_OK) rc = efs_kv_key_inode(sh,dent.ino,ki,&ni);
    if (rc == EFS_OK) rc = efs_kv_key_inode(sh,parent,kp,&np);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,kd,nd);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,ki,ni);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,kp,np);
    if (rc != EFS_OK) return rc;
    rc = efs_meta_apply_get_inode(kv,dent.ino,&row);
    if (rc != EFS_ERR_NOT_FOUND)
        return rc == EFS_OK ? EFS_ERR_STALE : rc;
    rc = efs_kv_key_dseq(sh,parent,0,ks,&ns);
    if (rc == EFS_OK) rc = efs_txn_key_busy(kv,ks,ns);
    if (rc == EFS_OK) rc = dseq_bump(kv,sh,parent,0,ks,&ns,vs);
    if (rc != EFS_OK) return rc;
    prow.base_mtime = max_u64(prow.base_mtime,now);
    prow.base_ctime = max_u64(prow.base_ctime,now);
    efs_meta_dir_note_entry(&prow,-1);
    pack_inode(vp,&prow);
    struct efs_kv_item it[4]; memset(it,0,sizeof(it));
    it[0]=(struct efs_kv_item){.op=EFS_KV_DEL,.key=kd,.klen=nd};
    it[1]=(struct efs_kv_item){.op=EFS_KV_PUT,.key=kp,.klen=np,.val=vp,.vlen=sizeof(vp)};
    it[2]=(struct efs_kv_item){.op=EFS_KV_PUT,.key=ks,.klen=ns,.val=vs,.vlen=sizeof(vs)};
    uint32_t n=3;
    if (q) {
        struct efs_opid_reply rep={0};rep.rc=EFS_OK;rep.ino=dent.ino;
        rc=opid_item(kv,sh,q,&rep,ko,vo,&it[n]);
        if (rc<0) return rc;
        n+=(uint32_t)rc;
    }
    return meta_write_batch(kv,it,n);
}

int efs_meta_apply_expiry_policy(struct efs_kv *kv, efs_ino_t root,
                                 uint64_t gen, uint32_t caller_uid,
                                 uint64_t expected,
                                 const struct efs_expiry_policy *policy)
{
    struct efs_meta_row row;
    int rc=efs_meta_apply_get_inode(kv,root,&row);
    if(rc!=EFS_OK)return rc;
    if(row.generation!=gen)return EFS_ERR_STALE;
    if(!S_ISDIR(row.mode))return EFS_ERR_INVAL;
    return efs_expiry_policy_set(kv,root,gen,caller_uid,expected,policy);
}

int efs_meta_apply_expiry_renew(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t gen, uint32_t caller_uid,
                                uint64_t expected, uint64_t now,
                                uint64_t deadline)
{
    struct efs_meta_row row;
    struct efs_expiry_record old,renewed;
    struct efs_expiry_policy policy;
    struct efs_expiry_change change;
    uint8_t inode_key[EFS_KV_KEY_MAX];uint32_t inode_len=0;
    int rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc!=EFS_OK)return rc;
    if(row.generation!=gen)return EFS_ERR_STALE;
    if(!row.nlink)return EFS_ERR_NOT_FOUND;
    rc=efs_kv_key_inode(efs_kv_inode_shard(ino),ino,inode_key,&inode_len);
    if(rc==EFS_OK)rc=efs_txn_key_busy(kv,inode_key,inode_len);
    if(rc!=EFS_OK)return rc;
    rc=efs_expiry_get(kv,ino,gen,&old);
    if(rc!=EFS_OK)return rc;
    if(!old.revision)return EFS_ERR_NODATA;
    if(old.revision!=expected)return EFS_ERR_STALE;
    if(efs_raft_shard_group(efs_kv_inode_shard(ino))!=
       efs_raft_shard_group(efs_kv_inode_shard(old.root)))return EFS_ERR_INVAL;
    uint8_t policy_key[EFS_EXPIRY_KEY_BYTES];
    efs_expiry_policy_key(old.root,old.root_generation,policy_key);
    rc=efs_txn_key_busy(kv,policy_key,sizeof(policy_key));
    if(rc==EFS_OK)rc=efs_expiry_policy_get(kv,old.root,old.root_generation,&policy);
    if(rc==EFS_OK)rc=efs_expiry_renew(&old,&policy,caller_uid,row.uid,
                                      row.nlink,row.mode,now,deadline,&renewed);
    if(rc==EFS_OK)rc=efs_expiry_plan(kv,ino,gen,expected,&renewed,&change);
    if(rc!=EFS_OK)return rc;
    return meta_write_batch(kv,change.items,change.count);
}

static int unlink_batch(struct efs_kv *kv, efs_ino_t parent, const char *name,
                             uint64_t now, const struct efs_opid_req *q,
                             const struct efs_expiry_due *expiry)
{
    struct efs_meta_dentry dent, tomb;
    struct efs_meta_row row, prow;
    struct efs_opid_reply rep;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL], v_tomb[DENT_VAL];
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint8_t k_opid[EFS_KV_KEY_MAX], v_opid[EFS_OPID_VAL_MAX];
    uint8_t k_xa[EFS_KV_KEY_MAX];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0, kr = 0, kxa = 0;
    struct efs_kv_item it[16];
    struct efs_expiry_change expiry_change;
    uint32_t n = 0, psh, hsh, dsh, nlink_after = 0;
    int rc, held = 0;
    int touch_parent = 0;
    int stamp_lane = 0;
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t ks = 0;
    uint8_t dseq_lane;

    /* I16 first: after a committed unlink the dentry is gone, and the
     * replay must answer OK, not ENOENT. The window shard is the dentry
     * shard, which needs the parent's layout. */
    if (q && opid_hosted(&q->id)) {
        rc = efs_meta_apply_get_inode(kv, parent, &prow);
        if (rc != EFS_OK)
            return rc;
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        rc = efs_meta_apply_opid_probe(kv, dsh, &q->id, &rep);
        if (rc < 0)
            return rc;
        if (rc)
            return rep.rc;
    }
    rc = efs_meta_apply_resolve(kv, parent, name, &dent, &row);
    if (expiry && rc != EFS_OK) return rc;
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_IO) {
        int repaired = apply_orphan_unlink(kv,parent,name,now,q);
        return repaired == EFS_ERR_NOT_FOUND ? rc : repaired;
    }
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    if (expiry) {
        struct efs_expiry_record current;
        if (!S_ISREG(row.mode) || row.nlink != 1 ||
            row.ino != expiry->ino || row.generation != expiry->generation)
            return EFS_ERR_STALE;
        rc = efs_expiry_get(kv, row.ino, row.generation, &current);
        if (rc != EFS_OK) return rc;
        if (!current.deadline_ns || current.deadline_ns > now ||
            current.deadline_ns != expiry->deadline_ns ||
            current.revision != expiry->revision || current.parent!=parent ||
            strcmp(current.name,name)) return EFS_ERR_STALE;
        /* D43: only while the source folder policy covers the deadline
         * (same group here; cross-group sources take the guarded txn). */
        {
            struct efs_meta_row root;struct efs_expiry_policy policy;
            if (efs_raft_shard_group(efs_kv_inode_shard(current.root)) !=
                efs_raft_shard_group(efs_kv_inode_shard(row.ino))) return EFS_ERR_INVAL;
            rc = efs_meta_apply_get_inode(kv, current.root, &root);
            if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
            if (rc != EFS_OK) return rc;
            if (!S_ISDIR(root.mode) || !root.nlink ||
                root.generation != current.root_generation) return EFS_ERR_STALE;
            rc = efs_expiry_policy_get(kv, current.root, current.root_generation, &policy);
            if (rc != EFS_OK) return rc;
            if (!policy.revision || !policy.enabled) return EFS_ERR_STALE;
        }
        /* Tombstone the revision in the namespace decision. Delayed renewal
         * must also verify the live inode, not resurrect a retired FileID. */
        current.deadline_ns = 0;
        rc = efs_expiry_plan(kv,row.ino,row.generation,current.revision,
                             &current,&expiry_change);
        if (rc != EFS_OK) return rc;
    }
    nlink_after = row.nlink <= 1 ? 0 : row.nlink - 1;
    rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    psh = efs_kv_inode_shard(parent);
    dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    /* This helper is one participant-log apply. Cross-group expiry must
     * take the guarded transaction path; never write another group's rows. */
    if (expiry && (efs_raft_shard_group(psh) != efs_raft_shard_group(dsh) ||
        efs_raft_shard_group(efs_kv_inode_shard(row.ino)) != efs_raft_shard_group(dsh)))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_dentry(psh, parent, name, k_loc, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, &kh);
    if (rc != EFS_OK)
        return rc;
    /* W36: the DENTRY is deleted unversioned below, so it needs the same
     * probe the inode row gets further down. A RENAME of this name holds
     * an EXCL DEL intent on the source dentry (and a PUT on the dest)
     * from PREPARE until RESOLVE; its inode-row REDUCE may not have
     * landed yet, so the k_ino probe alone let this unlink delete the
     * dentry and the row underneath the pending rename, whose RESOLVE
     * then PUT the dest dentry over a row that no longer existed
     * (`-?????????`, posix2 peer_rename_vs_unlink_src, 1 in 6). BUSY
     * here makes the client retry after the rename resolves, and the
     * retry finds the name gone → ENOENT, which is the right answer. */
    rc = efs_txn_key_busy(kv, k_loc, kl);
    if (rc == EFS_OK && !(kh == kl && memcmp(k_hash, k_loc, kl) == 0))
        rc = efs_txn_key_busy(kv, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    /* A lane-0 name during SPLITTING has k_hash == k_loc, and the tombstone
     * below is what has to survive; dropping the redundant delete keeps that
     * from depending on the order a batch applies its items in. In a LOCAL
     * directory the keys alias too, but nothing rewrites the key afterwards,
     * so there the delete is the whole operation. */
    if (prow.layout != EFS_META_LAYOUT_HASHED &&
        !(prow.layout == EFS_META_LAYOUT_SPLITTING &&
          kl == kh && memcmp(k_loc, k_hash, kl) == 0)) {
        it[n].op = EFS_KV_DEL;
        it[n].key = k_loc;
        it[n].klen = kl;
        n++;
    }
    if (prow.layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow.layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        pack_dentry(v_tomb, &tomb);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_hash;
        it[n].klen = kh;
        it[n].val = v_tomb;
        it[n].vlen = DENT_VAL;
        n++;
    } else if (prow.layout == EFS_META_LAYOUT_HASHED) {
        it[n].op = EFS_KV_DEL;
        it[n].key = k_hash;
        it[n].klen = kh;
        n++;
    }
    if (row.nlink <= 1) {
        held = efs_lease_any(kv, row.ino, row.generation);
        if (held < 0)
            return held;
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        /* A transaction in the middle of changing this row (a LINK's
         * nlink++ reduce, a RENAME's parent SET) must not lose to an
         * unversioned DEL — same no-wait rule as PREPARE: BUSY, retry. */
        rc = efs_txn_key_busy(kv, k_ino, ki);
        if (rc != EFS_OK)
            return rc;
        if (held) {
            row.nlink = 0;
            pack_inode(v_ino, &row);
            it[n].op = EFS_KV_PUT;
            it[n].key = k_ino;
            it[n].klen = ki;
            it[n].val = v_ino;
            it[n].vlen = INO_VAL;
            n++;
            rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(row.ino)),
                                 row.ino, k_reap, &kr);
            if (rc != EFS_OK) return rc;
            k_reap[2] = EFS_KV_KIND_ORPHAN;
            be64(v_reap, row.generation);
            it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_reap, kr, v_reap, 8};
        } else {
            int xa = 0;

            it[n].op = EFS_KV_DEL;
            it[n].key = k_ino;
            it[n].klen = ki;
            n++;
            rc = xattr_del_item(kv, row.ino, k_xa, &kxa, &it[n], &xa);
            if (rc != EFS_OK)
                return rc;
            n += (uint32_t)xa;
            /* The row dies here; the reap marker is what the background
             * reaper needs to sweep the dead file's lanes and GC its
             * fragments (L7), and it must be atomic with the delete. The
             * lease-held case above keeps the row, so the marker comes
             * from efs_meta_apply_reclaim when the last lease closes. */
            rc = efs_kv_key_reap(
                efs_kv_anchor_shard(efs_kv_inode_shard(row.ino)), row.ino,
                k_reap, &kr);
            if (rc != EFS_OK)
                return rc;
            efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
            it[n].op = EFS_KV_PUT;
            it[n].key = k_reap;
            it[n].klen = kr;
            it[n].val = v_reap;
            it[n].vlen = EFS_META_REAP_VAL;
            n++;
        }
    } else {
        /* Surviving links keep the inode; POSIX updates its ctime. */
        row.nlink--;
        row.base_ctime = max_u64(row.base_ctime, now);
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        pack_inode(v_ino, &row);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ino;
        it[n].klen = ki;
        it[n].val = v_ino;
        it[n].vlen = INO_VAL;
        n++;
    }
    /* Same split as create: LOCAL times ride the parent row; spread times
     * live on the dentry shard's dir lane so unlink does not re-serialize
     * on the directory's home leader. */
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        prow.base_mtime = max_u64(prow.base_mtime, now);
        prow.base_ctime = max_u64(prow.base_ctime, now);
        efs_meta_dir_note_entry(&prow, -1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&prow);
        rc = dir_lane_stamp(kv, &prow, name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &prow);
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    rc = dseq_bump(kv, dsh, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    if (q) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = row.ino;
        rep.extra = nlink_after;
        rc = opid_item(kv, dsh, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    if (expiry) {
        if (n + expiry_change.count > sizeof(it)/sizeof(it[0]))
            return EFS_ERR_OVERFLOW;
        for (uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
    }
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_unlink_op(struct efs_kv *kv, efs_ino_t parent, const char *name,
                             uint64_t now, const struct efs_opid_req *q)
{ return unlink_batch(kv,parent,name,now,q,NULL); }

int efs_meta_apply_expire(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          uint64_t committed_now, const struct efs_expiry_due *due)
{
    if (!due || !due->revision || !due->deadline_ns) return EFS_ERR_INVAL;
    return unlink_batch(kv,parent,name,committed_now,NULL,due);
}

int efs_meta_apply_link(struct efs_kv *kv, efs_ino_t src_parent, const char *src_name,
                        efs_ino_t dst_parent, const char *dst_name, uint64_t now)
{
    struct efs_meta_dentry src, ndent;
    struct efs_meta_row row, dprow;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_par[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_par[INO_VAL];
    uint8_t v_ln[LANE_VAL], v_dseq[8];
    uint32_t kd = 0, ki = 0, kp = 0, kln = 0, ks = 0;
    struct efs_kv_item it[8];
    uint32_t n = 0, dsh;
    uint64_t bit;
    uint8_t dseq_lane;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;

    if (!kv || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_resolve(kv, src_parent, src_name, &src, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    struct efs_expiry_record expiry;
    rc=efs_expiry_get(kv,row.ino,row.generation,&expiry);
    if(rc!=EFS_OK)return rc;
    if(expiry.deadline_ns)return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, dst_parent, dst_name, &ndent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    dseq_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0
                                                     : efs_kv_dir_lane(dst_name);
    memset(&ndent, 0, sizeof(ndent));
    ndent.ino = row.ino;
    ndent.generation = row.generation;
    ndent.type = row.mode & S_IFMT;
    pack_dentry(v_dent, &ndent);
    row.nlink++;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(v_ino, &row);

    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;

    if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
        dprow.base_mtime = max_u64(dprow.base_mtime, now);
        dprow.base_ctime = max_u64(dprow.base_ctime, now);
        efs_meta_dir_note_entry(&dprow, 1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&dprow);
        bit = 1ull << efs_kv_dir_lane(dst_name);
        if ((dprow.used_shards & bit) == 0) {
            dprow.used_shards |= bit;
            touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, &dprow, dst_name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &dprow);
        rc = efs_kv_key_inode(efs_kv_inode_shard(dst_parent), dst_parent, k_par,
                              &kp);
        if (rc != EFS_OK)
            return rc;
    }
    rc = dseq_bump(kv, dsh, dst_parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (touch_parent) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    return meta_write_batch(kv, it, n);
}

/* Drop one name from a directory into `it`, matching unlink's LOCAL /
 * SPLITTING / HASHED rules. `k_loc`/`k_hash`/`v_tomb` must outlive the batch. */
int dentry_drop_items(struct efs_kv *kv, const struct efs_meta_row *prow,
                             efs_ino_t parent, const char *name,
                             uint8_t *k_loc, uint32_t *kl, uint8_t *k_hash,
                             uint32_t *kh, uint8_t *v_tomb,
                             struct efs_kv_item *it, uint32_t *n)
{
    uint32_t psh, hsh;
    struct efs_meta_dentry tomb;
    int rc;

    (void)kv;
    psh = efs_kv_inode_shard(parent);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(psh, parent, name, k_loc, kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    if (prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING &&
          *kl == *kh && memcmp(k_loc, k_hash, *kl) == 0)) {
        it[*n].op = EFS_KV_DEL;
        it[*n].key = k_loc;
        it[*n].klen = *kl;
        (*n)++;
    }
    if (prow->layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow->layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        pack_dentry(v_tomb, &tomb);
        it[*n].op = EFS_KV_PUT;
        it[*n].key = k_hash;
        it[*n].klen = *kh;
        it[*n].val = v_tomb;
        it[*n].vlen = DENT_VAL;
        (*n)++;
    } else if (prow->layout == EFS_META_LAYOUT_HASHED) {
        it[*n].op = EFS_KV_DEL;
        it[*n].key = k_hash;
        it[*n].klen = *kh;
        (*n)++;
    }
    return EFS_OK;
}

static int stamp_dir_items(struct efs_kv *kv, struct efs_meta_row *prow,
                           efs_ino_t parent, const char *name, uint64_t now,
                           uint8_t *k_par, uint32_t *kp, uint8_t *v_par,
                           uint8_t *k_ln, uint32_t *kln, uint8_t *v_ln,
                           int *touch_parent, int *stamp_lane)
{
    uint64_t bit;
    int rc;

    *touch_parent = 0;
    *stamp_lane = 0;
    if (prow->layout == EFS_META_LAYOUT_LOCAL) {
        prow->base_mtime = max_u64(prow->base_mtime, now);
        prow->base_ctime = max_u64(prow->base_ctime, now);
        *touch_parent = 1;
        /* Caller sets nents via efs_meta_dir_note_entry before stamp. */
    } else {
        bit = 1ull << efs_kv_dir_lane(name);
        if ((prow->used_shards & bit) == 0) {
            prow->used_shards |= bit;
            *touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, prow, name, now, k_ln, kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        *stamp_lane = 1;
    }
    if (*touch_parent) {
        pack_inode(v_par, prow);
        rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, kp);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* POSIX: src must not be an ancestor of dst. Walking the live tree is
 * unsound under concurrency; the sim txn guards parent_version. This
 * helper is the single-KV form of the same predicate. */
static int dir_is_under(struct efs_kv *kv, efs_ino_t ancestor, efs_ino_t start)
{
    efs_ino_t cur = start;
    struct efs_meta_row r;
    int hops, rc;

    for (hops = 0; hops < 64; hops++) {
        if (cur == ancestor)
            return EFS_ERR_INVAL;
        rc = efs_meta_apply_get_inode(kv, cur, &r);
        if (rc != EFS_OK)
            return rc;
        if (!S_ISDIR(r.mode))
            return EFS_ERR_INVAL;
        if (cur == EFS_ROOT_INO || cur == r.parent)
            return EFS_OK;
        cur = r.parent;
    }
    return EFS_ERR_INVAL;
}

int efs_meta_apply_rename(struct efs_kv *kv, efs_ino_t src_parent,
                          const char *src_name, efs_ino_t dst_parent,
                          const char *dst_name, uint64_t now)
{
    struct efs_meta_dentry src, exist;
    struct efs_meta_row row, sprow, dprow;
    uint8_t k_sloc[EFS_KV_KEY_MAX], k_shash[EFS_KV_KEY_MAX], v_stomb[DENT_VAL];
    uint8_t k_dent[EFS_KV_KEY_MAX], v_dent[DENT_VAL];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL];
    uint8_t k_spar[EFS_KV_KEY_MAX], v_spar[INO_VAL];
    uint8_t k_dpar[EFS_KV_KEY_MAX], v_dpar[INO_VAL];
    uint8_t k_sln[EFS_KV_KEY_MAX], v_sln[LANE_VAL];
    uint8_t k_dln[EFS_KV_KEY_MAX], v_dln[LANE_VAL];
    uint8_t k_sdseq[EFS_KV_KEY_MAX], v_sdseq[8];
    uint8_t k_ddseq[EFS_KV_KEY_MAX], v_ddseq[8];
    uint8_t k_pver[EFS_KV_KEY_MAX], v_pver[8];
    uint32_t ksl = 0, ksh = 0, kd = 0, ki = 0, ksp = 0, kdp = 0;
    uint32_t ksln = 0, kdln = 0, kss = 0, kds = 0, kpv = 0;
    struct efs_kv_item it[16];
    struct efs_expiry_change expiry_change;int expiry_move=0;
    uint32_t n = 0, dsh;
    int rc, same_dir, is_dir, touch_src = 0, stamp_src = 0, touch_dst = 0,
        stamp_dst = 0;
    uint8_t s_lane, d_lane;

    if (!kv || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    if (src_parent == dst_parent && strcmp(src_name, dst_name) == 0)
        return EFS_OK;
    rc = efs_meta_apply_resolve(kv, src_parent, src_name, &src, &row);
    if (rc != EFS_OK)
        return rc;
    is_dir = S_ISDIR(row.mode);
    if (is_dir) {
        if (row.ino == EFS_ROOT_INO)
            return EFS_ERR_INVAL;
        rc = dir_is_under(kv, row.ino, dst_parent);
        if (rc != EFS_OK)
            return rc;
        row.parent_version++;
    }
    rc = efs_meta_apply_get_inode(kv, src_parent, &sprow);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, dst_parent, dst_name, &exist);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST; /* replace is a wider txn; not this helper */
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    if(S_ISREG(row.mode)) {
        struct efs_expiry_record expiry;
        rc=efs_expiry_get(kv,row.ino,row.generation,&expiry);
        if(rc!=EFS_OK)return rc;
        if(expiry.revision && row.nlink==1 &&
           (expiry.deadline_ns || (expiry.parent==src_parent && !strcmp(expiry.name,src_name)))) {
            if(expiry.parent!=src_parent || strcmp(expiry.name,src_name))return EFS_ERR_STALE;
            expiry.parent=dst_parent;memset(expiry.name,0,sizeof(expiry.name));
            memcpy(expiry.name,dst_name,strlen(dst_name)+1);
            rc=efs_expiry_plan(kv,row.ino,row.generation,expiry.revision,&expiry,&expiry_change);
            if(rc!=EFS_OK)return rc;
            expiry_move=1;
        }
    }
    same_dir = src_parent == dst_parent;
    if (is_dir && !same_dir) {
        if (sprow.nlink < 3)
            return EFS_ERR_PROTO;
        sprow.nlink--;
        dprow.nlink++;
    }
    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    s_lane = sprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(src_name);
    d_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(dst_name);
    memset(&exist, 0, sizeof(exist));
    exist.ino = row.ino;
    exist.generation = row.generation;
    exist.type = row.mode & S_IFMT;
    pack_dentry(v_dent, &exist);
    row.parent = dst_parent;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(v_ino, &row);
    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    rc = dentry_drop_items(kv, &sprow, src_parent, src_name, k_sloc, &ksl,
                           k_shash, &ksh, v_stomb, it, &n);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (is_dir) {
        rc = efs_kv_key_pver(efs_kv_inode_shard(row.ino), row.ino, k_pver, &kpv);
        if (rc != EFS_OK)
            return rc;
        be64(v_pver, row.parent_version);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_pver;
        it[n].klen = kpv;
        it[n].val = v_pver;
        it[n].vlen = 8;
        n++;
    }

    /* Apply rename does not replace (EXIST). same-dir is net-zero nents. */
    if (!same_dir)
        efs_meta_dir_note_entry(&sprow, -1);
    efs_dir_spread_seen(&sprow);
    rc = stamp_dir_items(kv, &sprow, src_parent, src_name, now, k_spar, &ksp,
                         v_spar, k_sln, &ksln, v_sln, &touch_src, &stamp_src);
    if (rc != EFS_OK)
        return rc;
    if (!same_dir) {
        efs_meta_dir_note_entry(&dprow, 1);
        efs_dir_spread_seen(&dprow);
        rc = stamp_dir_items(kv, &dprow, dst_parent, dst_name, now, k_dpar, &kdp,
                             v_dpar, k_dln, &kdln, v_dln, &touch_dst,
                             &stamp_dst);
        if (rc != EFS_OK)
            return rc;
    } else {
        /* One parent row: times already in sprow. Dest name may land on
         * a dir lane this directory has never used — that is first use,
         * so the used_shards bit belongs on the same PUT as src. */
        touch_dst = 0;
        stamp_dst = 0;
        if (dprow.layout != EFS_META_LAYOUT_LOCAL) {
            uint64_t bit = 1ull << efs_kv_dir_lane(dst_name);
            if ((sprow.used_shards & bit) == 0) {
                sprow.used_shards |= bit;
                pack_inode(v_spar, &sprow);
                rc = efs_kv_key_inode(efs_kv_inode_shard(src_parent), src_parent,
                                      k_spar, &ksp);
                if (rc != EFS_OK)
                    return rc;
                touch_src = 1;
            }
            rc = dir_lane_stamp(kv, &sprow, dst_name, now, k_dln, &kdln, v_dln);
            if (rc != EFS_OK)
                return rc;
            stamp_dst = 1;
        }
    }
    if (is_dir && !same_dir) {
        if (!touch_src) {
            pack_inode(v_spar, &sprow);
            rc = efs_kv_key_inode(efs_kv_inode_shard(src_parent), src_parent,
                                  k_spar, &ksp);
            if (rc != EFS_OK)
                return rc;
            touch_src = 1;
        }
        if (!touch_dst) {
            pack_inode(v_dpar, &dprow);
            rc = efs_kv_key_inode(efs_kv_inode_shard(dst_parent), dst_parent,
                                  k_dpar, &kdp);
            if (rc != EFS_OK)
                return rc;
            touch_dst = 1;
        }
    }
    if (touch_src) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_spar;
        it[n].klen = ksp;
        it[n].val = v_spar;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_src) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_sln;
        it[n].klen = ksln;
        it[n].val = v_sln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    if (touch_dst) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_dpar;
        it[n].klen = kdp;
        it[n].val = v_dpar;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_dst) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_dln;
        it[n].klen = kdln;
        it[n].val = v_dln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    rc = dseq_bump(kv, efs_kv_dentry_shard(src_parent, src_name, sprow.layout),
                   src_parent, s_lane, k_sdseq, &kss, v_sdseq);
    if (rc == EFS_OK)
        rc = dseq_bump(kv, dsh, dst_parent, d_lane, k_ddseq, &kds, v_ddseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_sdseq;
    it[n].klen = kss;
    it[n].val = v_sdseq;
    it[n].vlen = 8;
    n++;
    if (!(same_dir && sprow.layout == EFS_META_LAYOUT_LOCAL &&
          kss == kds && memcmp(k_sdseq, k_ddseq, kss) == 0)) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ddseq;
        it[n].klen = kds;
        it[n].val = v_ddseq;
        it[n].vlen = 8;
        n++;
    }
    if(expiry_move) {
        if(n+expiry_change.count>sizeof(it)/sizeof(it[0]))return EFS_ERR_OVERFLOW;
        for(uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
    }
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_setattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_setattr *sa)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0;
    int rc;

    if (!kv || !sa || ino == 0)
        return EFS_ERR_INVAL;
    if (sa->mask & ~(EFS_META_SET_MODE | EFS_META_SET_UID | EFS_META_SET_GID))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (sa->expect_gen != 0 && sa->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if (sa->mask & EFS_META_SET_MODE)
        row.mode = (row.mode & ~07777u) | (sa->mode & 07777u);
    if (sa->mask & EFS_META_SET_UID)
        row.uid = sa->uid;
    if (sa->mask & EFS_META_SET_GID)
        row.gid = sa->gid;
    /* A metadata change stamps ctime and leaves mtime alone. MAX-clamped, so
     * a backwards CLOCK_REALTIME cannot walk ctime back, and so re-applying
     * the same committed entry is a no-op. */
    row.base_ctime = max_u64(row.base_ctime, now);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    pack_inode(val, &row);
    return meta_write_put(kv, key, klen, val, INO_VAL);
}
