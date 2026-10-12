#ifndef EFS_TEST_RETAINED_NAMESPACE_AUDIT_H
#define EFS_TEST_RETAINED_NAMESPACE_AUDIT_H
/* Read-only, single pinned local KV image. Not a repair or quorum certificate. */
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include <string.h>
#include <sys/stat.h>
struct retained_audit {
    struct efs_meta_row inode, parent;
    uint64_t lineage[2], transaction_revision, records, limit;
    uint64_t kinds[256], raw_backlinks, authoritative_backlinks, children, directory_children;
    int local_scan_complete, binding_inconsistent, binding_inconclusive;
    int ownership_inconclusive, distributed_coherence_proven, repair_eligible;
};
struct retained_audit_ctx {
    struct efs_kv_lsm_snapshot *source;
    struct efs_kv kv;
    struct retained_audit *report;
    int (*cancel)(void *); void *cancel_ctx;
};
static int retained_get(void *ctx,const uint8_t *k,uint32_t n,uint8_t *v,uint32_t *vn)
{ return efs_kv_lsm_snapshot_get(ctx,k,n,v,vn); }
static const struct efs_kv_ops retained_read_ops={.get=retained_get};
static int retained_record(void *ctx,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{
    struct retained_audit_ctx *c=ctx;struct retained_audit *r=c->report;
    if(c->cancel && c->cancel(c->cancel_ctx))return EFS_ERR_BUSY;
    if(r->records>=r->limit)return EFS_ERR_OVERFLOW;
    r->records++;
    if(kn<3 || (((uint32_t)k[0]<<8)|k[1])>EFS_KV_SHARD_MASK)return EFS_ERR_PROTO;
    r->kinds[k[2]]++;
    if(k[2]!=EFS_KV_KIND_DENTRY)return EFS_OK;
    if(kn<=11 || kn-11>=EFS_MAX_NAME || memchr(k+11,0,kn-11) || memchr(k+11,'/',kn-11))return EFS_ERR_PROTO;
    struct efs_meta_dentry d;int rc=efs_meta_unpack_dentry(v,vn,&d);if(rc)return rc;
    if(!d.ino || d.type==EFS_META_DENT_TOMBSTONE)return EFS_OK;
    efs_ino_t parent=0;for(unsigned i=3;i<11;i++)parent=(parent<<8)|k[i];
    if(d.ino!=r->inode.ino && parent!=r->inode.ino)return EFS_OK;
    if(d.ino==r->inode.ino)r->raw_backlinks++;
    char name[EFS_MAX_NAME];memcpy(name,k+11,kn-11);name[kn-11]=0;
    struct efs_meta_dentry chosen;rc=efs_meta_apply_lookup(&c->kv,parent,name,&chosen);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_OK; /* inactive layout copy */
    if(rc==EFS_ERR_BUSY || rc==EFS_ERR_STALE){r->binding_inconclusive=1;return EFS_OK;}
    if(rc)return rc;
    if(chosen.ino!=d.ino || chosen.generation!=d.generation)return EFS_OK;
    /* Count only the actual layout-selected shard, not stale duplicate copies. */
    struct efs_meta_row p;rc=efs_meta_apply_get_inode(&c->kv,parent,&p);if(rc)return rc;
    uint32_t shard=((uint32_t)k[0]<<8)|k[1];
    if(p.layout==EFS_META_LAYOUT_SPLITTING){r->binding_inconclusive=1;return EFS_OK;}
    if(shard!=efs_kv_dentry_shard(parent,name,p.layout))return EFS_OK;
    if(parent==r->inode.ino) {
        r->children++;
        struct efs_meta_row child;
        rc=efs_meta_apply_get_inode(&c->kv,d.ino,&child);
        if(rc==EFS_ERR_NOT_FOUND)r->binding_inconsistent=1;
        else if(rc)return rc;
        else {
            if(!child.nlink || child.generation!=d.generation ||
               (S_ISDIR(child.mode) && child.parent!=parent))r->binding_inconsistent=1;
            if(S_ISDIR(child.mode))r->directory_children++;
        }
    }
    if(d.ino==r->inode.ino) {
        if(parent!=r->inode.parent || d.generation!=r->inode.generation)r->binding_inconsistent=1;
        else r->authoritative_backlinks++;
    }
    return EFS_OK;
}
static int retained_namespace_audit(struct efs_kv_lsm_snapshot *source,efs_ino_t ino,
    uint64_t max_records,int (*cancel)(void *),void *cancel_ctx,struct retained_audit *out)
{
    if(!source || !out || !max_records)return EFS_ERR_INVAL;
    memset(out,0,sizeof(*out));out->limit=max_records;
    /* Ownership/replica coverage remains inconclusive even if no rows exist:
     * the reader cannot certify session fencing or a distributed apply cut. */
    out->ownership_inconclusive=1;
    struct retained_audit_ctx c={.source=source,.kv={&retained_read_ops,source},.report=out,.cancel=cancel,.cancel_ctx=cancel_ctx};
    if(cancel && cancel(cancel_ctx))return EFS_ERR_BUSY;
    int rc=efs_meta_apply_get_inode(&c.kv,ino,&out->inode);if(rc)return rc;
    rc=efs_meta_apply_get_inode(&c.kv,out->inode.parent,&out->parent);if(rc)return rc;
    uint64_t revisions[4096];rc=efs_kv_lsm_snapshot_query_revisions(source,out->lineage,revisions);if(rc)return rc;
    rc=efs_kv_lsm_snapshot_transaction_revision(source,&out->transaction_revision);if(rc)return rc;
    rc=efs_kv_lsm_snapshot_scan(source,NULL,0,NULL,0,retained_record,&c);if(rc)return rc;
    out->local_scan_complete=1;
    if(out->kinds[EFS_KV_KIND_INTENT] || out->kinds[EFS_KV_KIND_GUARD] || out->kinds[EFS_KV_KIND_REDUCE])out->binding_inconclusive=1;
    if(out->inode.nlink && S_ISDIR(out->inode.mode) && ino!=EFS_ROOT_INO &&
       out->authoritative_backlinks!=1)out->binding_inconsistent=1;
    if(out->inode.nlink && S_ISDIR(out->inode.mode) &&
       out->inode.nlink!=2+out->directory_children)out->binding_inconsistent=1;
    return EFS_OK;
}
#endif
