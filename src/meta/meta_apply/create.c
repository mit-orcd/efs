/* meta_apply/create.c: CREATE and MKDIR (local and log paths). */

#include "apply_internal.h"
#include "rows.h"
#include "init.h"

/* Resolve nearest explicit policy under one participant's log. This staged
 * helper refuses another group's ancestors, rather than reading follower
 * state as policy authority. Cross-group inheritance needs a guarded txn. */
int efs_meta_apply_create_refusal(struct efs_kv *kv,uint32_t shard,
                                  const struct efs_opid_req *q)
{
    if(!q || !efs_opid_req_valid(q) || !opid_hosted(&q->id) ||
       shard>EFS_KV_SHARD_MASK)return EFS_ERR_INVAL;
    struct efs_opid_reply reply;int rc=efs_meta_apply_opid_probe(kv,shard,&q->id,&reply);
    if(rc<0)return rc;
    if(rc)return reply.rc;
    rc=efs_session_accept(kv,shard,q->id.client_uuid,q->id.session_epoch);
    if(rc!=EFS_OK)return rc;
    uint8_t k[EFS_KV_KEY_MAX],v[EFS_OPID_VAL_MAX];struct efs_kv_item item;
    memset(&reply,0,sizeof(reply));reply.rc=EFS_ERR_EXIST;
    rc=opid_item(kv,shard,q,&reply,k,v,&item);
    if(rc<0)return rc;
    if(rc!=1)return EFS_ERR_BUSY;
    rc=meta_write_batch(kv,&item,1);return rc==EFS_OK?EFS_ERR_EXIST:rc;
}

static int expiry_local_default(struct efs_kv *kv, efs_ino_t parent,
                                 uint8_t group, uint64_t now,
                                 struct efs_expiry_record *out)
{
    efs_ino_t seen[EFS_META_PATH_MAX];uint32_t count=0;
    memset(out,0,sizeof(*out));
    while(parent) {
        if(count==EFS_META_PATH_MAX)return EFS_ERR_OVERFLOW;
        for(uint32_t i=0;i<count;i++)if(seen[i]==parent)return EFS_ERR_IO;
        seen[count++]=parent;
        if(efs_raft_shard_group(efs_kv_inode_shard(parent))!=group)
            return EFS_ERR_INVAL;
        struct efs_meta_row row;struct efs_expiry_policy policy;
        uint8_t k[EFS_KV_KEY_MAX];uint32_t kn=0;
        int rc=efs_kv_key_inode(efs_kv_inode_shard(parent),parent,k,&kn);
        if(rc==EFS_OK)rc=efs_txn_key_busy(kv,k,kn);
        if(rc==EFS_OK)rc=efs_meta_apply_get_inode(kv,parent,&row);
        if(rc!=EFS_OK)return rc;
        if(!S_ISDIR(row.mode) || !row.nlink)return EFS_ERR_NOT_FOUND;
        efs_expiry_policy_key(parent,row.generation,k);
        rc=efs_txn_key_busy(kv,k,EFS_EXPIRY_KEY_BYTES);
        if(rc==EFS_OK)rc=efs_expiry_policy_get(kv,parent,row.generation,&policy);
        if(rc!=EFS_OK)return rc;
        if(policy.revision)
            return efs_expiry_materialize(&policy,parent,row.generation,now,out);
        if(parent==EFS_ROOT_INO)return EFS_OK;
        parent=row.parent;
    }
    return EFS_ERR_IO;
}

static int create_file_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                             efs_ino_t parent, uint32_t mode,
                             const char *name, const struct efs_opid_req *q,
                             int from_log, efs_ino_t want_ino, int want_layout,
                             const uint8_t *lease_uuid, uint32_t lease_epoch,
                             uint64_t lease_token, int expiry_enabled, efs_ino_t *out)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, parent_row;
    struct efs_opid_reply rep;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_opid[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_opid[EFS_OPID_VAL_MAX];
    uint32_t kd = 0, ki = 0, ka = 0;
    struct efs_kv_item it[14];
    struct efs_expiry_change expiry_change;
    struct efs_expiry_record expiry_record;
    int has_expiry=0;
    uint8_t k_lease[EFS_KV_KEY_MAX], lease_one = 1;
    uint8_t lease_pending[EFS_LEASE_PENDING_VALUE_LEN];
    uint32_t lease_len = 0;
    uint32_t n = 0, shard;
    efs_ino_t next = 0, ino;
    uint8_t k_aver[EFS_KV_KEY_MAX], v_aver[8];
    uint32_t kav = 0;
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t kp = 0, kln = 0, ks = 0;
    uint8_t dseq_lane;
    uint64_t bit;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;
    int remote_parent = 0;

    if (!kv || !name || !at || parent == 0)
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) == S_IFDIR)
        return EFS_ERR_INVAL; /* MKDIR is a 2-shard txn; not this helper */
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc == EFS_ERR_NOT_FOUND) {
        uint8_t pgrp = efs_raft_shard_group(efs_kv_inode_shard(parent));
        uint32_t hdsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        int hashed = (pgrp != efs_raft_shard_group(hdsh));
        int lay;

        /* A committed log entry must write the child even when this replica
         * lacks the parent row (follower lag / dual-host apply). A helper
         * call still treats same-group missing parent as a hole.
         * Do not guess HASHED from a missing parent on the log path — a
         * dual-host replica that can see a LOCAL parent would write
         * different keys. Packed layout/ino from the leader wins. */
        if (from_log)
            lay = want_layout >= 0 ? want_layout : EFS_META_LAYOUT_LOCAL;
        else if (hashed)
            lay = EFS_META_LAYOUT_HASHED;
        else
            lay = -1;
        if (lay >= 0) {
            memset(&parent_row, 0, sizeof(parent_row));
            parent_row.ino = parent;
            parent_row.generation = 1;
            parent_row.mode = S_IFDIR | 0755;
            parent_row.layout = (uint8_t)lay;
            parent_row.used_shards = (lay == EFS_META_LAYOUT_LOCAL)
                                         ? 0
                                         : ~(uint64_t)0;
            remote_parent = 1;
            rc = EFS_OK;
        }
    }
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(parent_row.mode))
        return EFS_ERR_INVAL;

    {
        uint8_t lay = want_layout >= 0 ? (uint8_t)want_layout
                                       : parent_row.layout;

        shard = efs_kv_dentry_shard(parent, name, lay);
        dseq_lane = lay == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    }
    /* Lease ownership (pending or CREATE-time open) is a session mutation,
     * including negative verdicts and replays. Check inside serialized apply,
     * before the replay fast path: an earlier local fence must prevent any
     * old-epoch CREATE claim. */
    if (lease_uuid) {
        rc = efs_session_accept(kv, shard, lease_uuid, lease_epoch);
        if (rc != EFS_OK) return rc;
    }
    /* I16: a replay of an op this window already recorded returns the
     * recorded verdict and touches nothing. */
    if (q) {
        int hit = efs_meta_apply_opid_probe(kv, shard, &q->id, &rep);

        if (hit < 0)
            return hit;
        if (hit) {
            if (lease_token && rep.rc == EFS_OK && !rep.ino)
                return EFS_ERR_STALE; /* retired reply cannot grant adoption */
            if (out)
                *out = rep.ino;
            return rep.rc;
        }
    }

    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc == EFS_OK) {
        if (q && opid_hosted(&q->id)) {
            /* A CREATE answered EEXIST must not become a successful new
             * CREATE when a delayed retry arrives after the name is removed.
             * Publish the negative verdict without namespace/lease effects. */
            memset(&rep, 0, sizeof(rep));
            rep.rc = EFS_ERR_EXIST;
            rc = opid_item(kv, shard, q, &rep, k_opid, v_opid, &it[0]);
            if (rc < 0) return rc;
            if (rc != 1) return EFS_ERR_BUSY;
            rc = meta_write_batch(kv, it, 1);
            if (rc != EFS_OK) return rc;
        }
        return EFS_ERR_EXIST;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (want_ino) {
        rc = alloc_hint_or_next(kv, shard, want_ino, &ino, &next);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = alloc_next_free(kv, shard, &ino, &next);
        if (rc != EFS_OK)
            return rc;
        if (efs_kv_inode_shard(ino) != shard)
            return EFS_ERR_PROTO;
    }

    memset(&row, 0, sizeof(row));
    row.ino = ino;
    row.generation = 1;
    row.mode = mode;
    row.nlink = 1;
    row.parent = parent;
    row.uid = at->uid;
    row.gid = at->gid;
    row.base_mtime = at->now;
    row.base_atime = at->now;
    row.base_ctime = at->now;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = row.generation;
    dent.type = mode & S_IFMT;
    if(expiry_enabled && S_ISREG(mode)) {
        rc=expiry_local_default(kv,parent,efs_raft_shard_group(shard),
                                 at->now,&expiry_record);
        if(rc!=EFS_OK)return rc;
        if(expiry_record.deadline_ns) {
            expiry_record.parent=parent;
            memcpy(expiry_record.name,name,strlen(name)+1);
            rc=efs_expiry_plan(kv,ino,row.generation,0,&expiry_record,&expiry_change);
            if(rc!=EFS_OK)return rc;
            has_expiry=1;
        }
    }
    pack_inode(v_ino, &row);
    pack_dentry(v_dent, &dent);
    be64(v_alloc, next);

    rc = efs_kv_key_dentry(shard, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(shard, ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(shard, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = alloc_key_claim(kv, k_alloc, ka, k_aver, &kav, v_aver);
    if (rc != EFS_OK)
        return rc;

    /* POSIX: adding an entry moves the containing directory's mtime and
     * ctime. For a LOCAL directory the parent row is on this very shard, so
     * it rides the same atomic batch and costs nothing. For a spread
     * directory it deliberately does NOT go here — routing every create back
     * to the parent's shard is the hotspot the spread exists to remove, so
     * those times live in a per-dentry-shard dir lane (§7.4). */
    if (remote_parent) {
        /* Parent row is on another Raft group. Write child + hashed
         * dentry only — a fabricated parent PUT would pollute this
         * namespace, and a lane stamp needs the real generation. */
    } else if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        efs_meta_dir_note_entry(&parent_row, 1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&parent_row);
        bit = 1ull << efs_kv_dir_lane(name);
        if ((parent_row.used_shards & bit) == 0) {
            parent_row.used_shards |= bit;
            touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, &parent_row, name, at->now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &parent_row);
        rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, &kp);
        if (rc != EFS_OK)
            return rc;
    }

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
    it[n].op = EFS_KV_PUT;
    it[n].key = k_alloc;
    it[n].klen = ka;
    it[n].val = v_alloc;
    it[n].vlen = ALLOC_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_aver;
    it[n].klen = kav;
    it[n].val = v_aver;
    it[n].vlen = 8;
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
    rc = dseq_bump(kv, shard, parent, dseq_lane, k_dseq, &ks, v_dseq);
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
        rep.ino = ino;
        rc = opid_item(kv, shard, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        if (opid_hosted(&q->id) && rc != 1)
            return EFS_ERR_BUSY; /* never create without its replay verdict */
        n += (uint32_t)rc;
    }

    if (lease_uuid) {
        rc = efs_kv_key_lease(shard, ino, row.generation, lease_uuid,
                               lease_epoch, k_lease, &lease_len);
        if (rc != EFS_OK) return rc;
        if (lease_token) {
            rc = efs_lease_pending_value(lease_token, lease_pending);
            if (rc != EFS_OK) return rc;
        }
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, k_lease, lease_len,
                         lease_token ? lease_pending : &lease_one,
                         lease_token ? EFS_LEASE_PENDING_VALUE_LEN : 1};
    }

    if(has_expiry) {
        if(n+expiry_change.count>sizeof(it)/sizeof(it[0]))return EFS_ERR_OVERFLOW;
        for(uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
    }
    rc = meta_write_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = ino;
    return EFS_OK;
}

int efs_meta_apply_create_file(struct efs_kv *kv, const struct efs_meta_attrs *at,
                               efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, 0, 0, -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  const struct efs_meta_attrs *at,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out)
{
    struct efs_opid_req q;

    if (!op)
        return EFS_ERR_INVAL;
    memset(&q, 0, sizeof(q));
    q.id = *op;
    return create_file_batch(kv, at, parent, mode, name, &q, 0, 0, -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_log(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                   efs_ino_t parent, uint32_t mode,
                                   const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, 1, 0, -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_log_op(struct efs_kv *kv,
                                      const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      int layout, const struct efs_opid_req *q,
                                      efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, q, 1, ino,
                             ino ? layout : -1, NULL, 0, 0, 0, out);
}

int efs_meta_apply_create_file_log_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      uint8_t layout, efs_ino_t *out)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, NULL, 1, ino,
                             (int)layout, NULL, 0, 0, 0, out);
}

/* Namespace, inode, replay verdict and initial lease share one KV batch.
 * Replaying an already recorded op never recreates a subsequently closed lease. */
int efs_meta_apply_create_file_log_lease(struct efs_kv *kv,
                                         const struct efs_meta_attrs *at,
                                         efs_ino_t parent, uint32_t mode,
                                         const char *name, efs_ino_t ino,
                                         int layout, const struct efs_opid_req *q,
                                         const uint8_t *uuid, uint32_t epoch,
                                         efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, q, 1, ino,
                             ino ? layout : -1, uuid, epoch, 0, 0, out);
}

/* Pending ownership: identity and token come from the same durable CREATE
 * request. The opt-in daemon form requires an established shard session;
 * FUSE adoption remains staged. Replay never reacquires ownership. */
int efs_meta_apply_create_file_log_pending(struct efs_kv *kv,
                                         const struct efs_meta_attrs *at,
                                         efs_ino_t parent, uint32_t mode,
                                         const char *name, efs_ino_t ino,
                                         int layout, const struct efs_opid_req *q,
                                         const uint8_t *uuid, uint32_t epoch,
                                         efs_ino_t *out)
{
    if (!q || !efs_opid_req_valid(q) || q->ack >= q->id.seq || !uuid || !epoch ||
        q->id.session_epoch != epoch ||
        memcmp(q->id.client_uuid, uuid, EFS_OPID_UUID_LEN) != 0)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, q, 1, ino,
                             ino ? layout : -1, uuid, epoch, q->id.seq, 0, out);
}

int efs_meta_apply_create_expiring(struct efs_kv *kv,
                                     const struct efs_meta_attrs *at,
                                     efs_ino_t parent,uint32_t mode,
                                     const char *name,efs_ino_t *out)
{
    return create_file_batch(kv,at,parent,mode,name,NULL,0,0,-1,NULL,0,0,1,out);
}

static int mkdir_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                       efs_ino_t parent, uint32_t mode, const char *name,
                       int from_log, efs_ino_t want_ino, int want_layout,
                       const struct efs_opid_req *q, efs_ino_t *out)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, parent_row;
    struct efs_opid_reply rep;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t k_opid[EFS_KV_KEY_MAX], v_opid[EFS_OPID_VAL_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_par[INO_VAL], v_ln[LANE_VAL], v_dseq[8];
    uint32_t kd = 0, ki = 0, ka = 0, kp = 0, kln = 0, ks = 0;
    uint8_t k_aver[EFS_KV_KEY_MAX], v_aver[8];
    uint32_t kav = 0;
    struct efs_kv_item it[10];
    uint32_t n = 0, psh, csh, dsh;
    efs_ino_t next = 0, ino;
    uint64_t salt = 0, bit;
    uint8_t dseq_lane, lay;
    int rc;
    int stamp_lane = 0;
    int remote_parent = 0;
    int touch_parent = 0;

    if (!kv || !name || !at || parent == 0 || name[0] == '\0')
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) != S_IFDIR)
        return EFS_ERR_INVAL;
    if ((mode & 07777) == 0)
        mode |= 0755;
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc == EFS_ERR_NOT_FOUND && from_log) {
        memset(&parent_row, 0, sizeof(parent_row));
        parent_row.ino = parent;
        parent_row.generation = 1;
        parent_row.mode = S_IFDIR | 0755;
        parent_row.nlink = 2;
        lay = want_layout >= 0 ? (uint8_t)want_layout : EFS_META_LAYOUT_LOCAL;
        parent_row.layout = lay;
        parent_row.used_shards = (lay == EFS_META_LAYOUT_LOCAL)
                                     ? 0
                                     : ~(uint64_t)0;
        remote_parent = 1;
        rc = EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(parent_row.mode))
        return EFS_ERR_INVAL;
    if (parent_row.layout != EFS_META_LAYOUT_LOCAL &&
        parent_row.layout != EFS_META_LAYOUT_HASHED &&
        parent_row.layout != EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_INVAL;
    lay = want_layout >= 0 ? (uint8_t)want_layout : parent_row.layout;
    dsh = efs_kv_dentry_shard(parent, name, lay);
    if (q) {
        int hit = efs_meta_apply_opid_probe(kv, dsh, &q->id, &rep);

        if (hit < 0)
            return hit;
        if (hit) {
            if (out)
                *out = rep.ino;
            return rep.rc;
        }
    }
    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    rc = efs_meta_apply_export_salt(kv, &salt);
    if (rc != EFS_OK)
        return rc;
    psh = efs_kv_inode_shard(parent);
    csh = efs_kv_mkdir_shard(parent, name, salt);
    /* A committed CREATE records the chosen FileID. Replay older scattered
     * allocations exactly, rather than relocating an inode or selecting a
     * different offset on a follower. Reject unrelated shard hints below. */
    if (from_log && want_ino && efs_kv_inode_shard(want_ino) != csh &&
        efs_kv_inode_shard(want_ino) ==
            efs_kv_mkdir_scattered_shard(parent, name, salt))
        csh = efs_kv_inode_shard(want_ino);
    dseq_lane = lay == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    if (want_ino) {
        rc = alloc_hint_or_next(kv, csh, want_ino, &ino, &next);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = alloc_next_free(kv, csh, &ino, &next);
        if (rc != EFS_OK)
            return rc;
        if (efs_kv_inode_shard(ino) != csh)
            return EFS_ERR_PROTO;
    }

    memset(&row, 0, sizeof(row));
    row.ino = ino;
    row.generation = 1;
    row.mode = mode;
    row.nlink = 2;
    row.parent = parent;
    row.uid = at->uid;
    row.gid = at->gid;
    row.base_mtime = at->now;
    row.base_atime = at->now;
    row.base_ctime = at->now;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = 1;
    dent.type = S_IFDIR;
    if (remote_parent) {
        /* Hollow follower: do not PUT a fabricated parent. */
    } else if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.nlink++;
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        efs_meta_dir_note_entry(&parent_row, 1);
        touch_parent = 1;
    } else {
        parent_row.nlink++;
        bit = 1ull << efs_kv_dir_lane(name);
        if ((parent_row.used_shards & bit) == 0)
            parent_row.used_shards |= bit;
        rc = dir_lane_stamp(kv, &parent_row, name, at->now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
        touch_parent = 1;
    }
    pack_inode(v_ino, &row);
    pack_dentry(v_dent, &dent);
    be64(v_alloc, next);
    rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = alloc_key_claim(kv, k_alloc, ka, k_aver, &kav, v_aver);
    if (rc == EFS_OK && touch_parent) {
        pack_inode(v_par, &parent_row);
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    }
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
    it[n].op = EFS_KV_PUT;
    it[n].key = k_alloc;
    it[n].klen = ka;
    it[n].val = v_alloc;
    it[n].vlen = ALLOC_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_aver;
    it[n].klen = kav;
    it[n].val = v_aver;
    it[n].vlen = 8;
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
        rep.ino = ino;
        rc = opid_item(kv, dsh, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    rc = meta_write_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = ino;
    return EFS_OK;
}

int efs_meta_apply_mkdir(struct efs_kv *kv, const struct efs_meta_attrs *at,
                         efs_ino_t parent, uint32_t mode, const char *name,
                         efs_ino_t *out)
{
    return mkdir_batch(kv, at, parent, mode, name, 0, 0, -1, NULL, out);
}

int efs_meta_apply_mkdir_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                            efs_ino_t parent, uint32_t mode, const char *name,
                            efs_ino_t ino, uint8_t layout, efs_ino_t *out)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return mkdir_batch(kv, at, parent, mode, name, 1, ino, (int)layout, NULL,
                       out);
}

int efs_meta_apply_mkdir_log_op(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                efs_ino_t parent, uint32_t mode, const char *name,
                                efs_ino_t ino, int layout,
                                const struct efs_opid_req *q, efs_ino_t *out)
{
    /* Without a leader-chosen ino this is the plain (non-log) mkdir shape
     * the host used before; the log-only parent stand-in needs the layout. */
    return mkdir_batch(kv, at, parent, mode, name, ino ? 1 : 0, ino,
                       ino ? layout : -1, q, out);
}
