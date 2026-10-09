/* Transactional APPEND completion. Reuse the applied-state state machine to
 * capture exact replacement images, rather than duplicating its frontier,
 * hole and DONE-row cleanup rules. The capture KV never mutates its base. */
#include "efs/meta_apply.h"
#include "efs/session.h"
#include <stdlib.h>
#include <string.h>

struct finish_image {
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t klen, expected_len, new_len;
    int op;
    uint8_t *expected, *replacement;
};
struct finish_capture {
    struct efs_kv *base;
    struct finish_image *images;
    uint32_t count, capacity, mutations;
    efs_ino_t close_ino;
};
static int finish_data_key(const uint8_t *key,uint32_t n)
{
    if(n<11)return 0;
    return key[2]==EFS_KV_KIND_INODE || key[2]==EFS_KV_KIND_LANE ||
           key[2]==EFS_KV_KIND_APPEND_CUR || key[2]==EFS_KV_KIND_APPEND_RSV ||
           key[2]==EFS_KV_KIND_CLOSE_STEP;
}
static int finish_record(struct finish_capture *c,const uint8_t *key,uint32_t n,
                           const uint8_t *value,uint32_t length,uint32_t *slot)
{
    if(n>EFS_KV_KEY_MAX || (length!=EFS_TXN_ABSENT && length>EFS_OPID_VAL_MAX))
        return EFS_ERR_INVAL;
    for(uint32_t i=0;i<c->count;++i) {
        struct finish_image *im=&c->images[i];
        if(im->klen!=n || memcmp(im->key,key,n))continue;
        if(im->expected_len!=length ||
           (length!=EFS_TXN_ABSENT && length && memcmp(im->expected,value,length)))
            return EFS_ERR_BUSY; /* source moved while collecting */
        *slot=i;return EFS_OK;
    }
    if(c->count>=EFS_META_LANES+5)return EFS_ERR_OVERFLOW;
    if(c->count==c->capacity) {
        uint32_t cap=c->capacity?c->capacity*2:16;
        if(cap>EFS_META_LANES+5)cap=EFS_META_LANES+5;
        size_t bytes=(size_t)cap*sizeof(*c->images);
        if(cap<c->capacity || bytes/sizeof(*c->images)!=cap)return EFS_ERR_NOMEM;
        struct finish_image *images=realloc(c->images,bytes);
        if(!images)return EFS_ERR_NOMEM;
        c->images=images;c->capacity=cap;
    }
    struct finish_image *im=&c->images[c->count];memset(im,0,sizeof(*im));
    memcpy(im->key,key,n);im->klen=n;im->expected_len=length;
    im->op=length==EFS_TXN_ABSENT?EFS_TXN_DEL:EFS_TXN_PUT;
    im->new_len=length==EFS_TXN_ABSENT?0:length;
    if(length!=EFS_TXN_ABSENT && length) {
        im->expected=malloc(length);if(!im->expected)return EFS_ERR_NOMEM;
        memcpy(im->expected,value,length);
    }
    *slot=c->count++;return EFS_OK;
}
static int finish_get(void *ctx,const uint8_t *key,uint32_t n,uint8_t *value,uint32_t *length)
{
    struct finish_capture *c=ctx;
    int rc=efs_kv_get(c->base,key,n,value,length);
    if((rc==EFS_OK || rc==EFS_ERR_NOT_FOUND) && finish_data_key(key,n)) {
        uint32_t slot;
        int saved=finish_record(c,key,n,value,rc==EFS_OK?*length:EFS_TXN_ABSENT,&slot);
        if(saved!=EFS_OK)return saved;
    }
    return rc;
}
static int finish_scan(void *ctx,const uint8_t *prefix,uint32_t n,
                         int (*callback)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    struct finish_capture *c=ctx;
    /* The captured cursor and inode guard own reservation-set changes.
     * Materialized cursor reads and scans must stay in the same domain. */
    return efs_kv_scan_prefix(c->base,prefix,n,callback,user);
}
static int finish_scan_from(void *ctx,const uint8_t *prefix,uint32_t n,
                              const uint8_t *start,uint32_t sn,
                              int (*callback)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    struct finish_capture *c=ctx;
    return efs_kv_scan_from(c->base,prefix,n,start,sn,callback,user);
}
static int finish_batch(void *ctx,const struct efs_kv_item *items,uint32_t count)
{
    struct finish_capture *c=ctx;
    if(c->mutations || !count)return EFS_ERR_INVAL;
    for(uint32_t i=0;i<count;++i) {
        const struct efs_kv_item *q=&items[i];
        uint8_t old[EFS_OPID_VAL_MAX];uint32_t n=sizeof(old),slot;
        if(!finish_data_key(q->key,q->klen) ||
           (q->op!=EFS_KV_PUT && q->op!=EFS_KV_DEL) || q->vlen>sizeof(old))return EFS_ERR_INVAL;
        int rc=efs_kv_get(c->base,q->key,q->klen,old,&n);
        if(rc!=EFS_OK && rc!=EFS_ERR_NOT_FOUND)return rc;
        rc=finish_record(c,q->key,q->klen,old,rc==EFS_OK?n:EFS_TXN_ABSENT,&slot);
        if(rc!=EFS_OK)return rc;
        struct finish_image *im=&c->images[slot];
        if(im->replacement)return EFS_ERR_INVAL;
        im->op=q->op==EFS_KV_PUT?EFS_TXN_PUT:EFS_TXN_DEL;
        im->new_len=q->op==EFS_KV_PUT?q->vlen:0;
        if(im->new_len) {
            im->replacement=malloc(im->new_len);if(!im->replacement)return EFS_ERR_NOMEM;
            memcpy(im->replacement,q->val,im->new_len);
        }
    }
    c->mutations=count;return EFS_OK;
}
static int finish_execute(struct finish_capture *capture,const struct efs_txid *t,
                            const struct efs_meta_append_ops *ops,void *ctx)
{
    struct finish_capture c=*capture;
    struct efs_txn_parts parts={0};
    int rc=EFS_OK;
    /* The first read is the inode. Freeze it before any cursor or lane,
     * preventing activation, legacy reserve/resolve and truncate races. */
    if(!c.count || c.images[0].klen!=11 ||
       c.images[0].key[2]!=(c.close_ino?EFS_KV_KIND_CLOSE_STEP:EFS_KV_KIND_INODE)) {
        rc=EFS_ERR_INVAL;goto out;
    }
    if(c.count>EFS_META_LANES+4+(c.close_ino!=0)){rc=EFS_ERR_INVAL;goto out;}
    for(uint32_t i=0;i<c.count;++i) {
        struct finish_image *im=&c.images[i];
        if(memcmp(im->key+3,c.images[0].key+3,8)){rc=EFS_ERR_INVAL;goto out;}
        uint32_t shard=((uint32_t)im->key[0]<<8)|im->key[1],j,images=0;
        for(uint32_t k=0;k<c.count;++k)
            if(c.images[k].key[0]==im->key[0] && c.images[k].key[1]==im->key[1])++images;
        if(images>12){rc=EFS_ERR_INVAL;goto out;}
        for(j=0;j<parts.n;++j)if(parts.shard[j]==shard)break;
        if(j==parts.n) {
            if(parts.n==EFS_TXN_MAX_PART){rc=EFS_ERR_INVAL;goto out;}
            parts.shard[parts.n++]=shard;
        }
    }
    for(uint32_t i=0;i<c.count;++i) {
        struct finish_image *im=&c.images[i];struct efs_meta_append_image q={0};
        memcpy(q.key,im->key,im->klen);q.klen=im->klen;
        q.op=im->op;q.expected_len=im->expected_len;q.new_len=im->new_len;
        if(im->expected_len!=EFS_TXN_ABSENT && im->expected_len)
            memcpy(q.expected,im->expected,im->expected_len);
        if(im->new_len)memcpy(q.value,im->replacement?im->replacement:im->expected,im->new_len);
        rc=ops->prepare(ctx,t,&parts,&q);
        if(rc==EFS_OK && !i && c.close_ino) {
            /* Deferred OPEN/CLOSE checks this prepared obligation. A lease
             * acquired after capture but before PREPARE must stop cleanup. */
            int held=efs_lease_any(c.base,c.close_ino,
                (uint64_t)q.expected[0]<<56 | (uint64_t)q.expected[1]<<48 |
                (uint64_t)q.expected[2]<<40 | (uint64_t)q.expected[3]<<32 |
                (uint64_t)q.expected[4]<<24 | (uint64_t)q.expected[5]<<16 |
                (uint64_t)q.expected[6]<<8 | q.expected[7]);
            rc=held<0?held:held?EFS_ERR_BUSY:EFS_OK;
        }
        if(rc!=EFS_OK) {
            int aborted=ops->decide(ctx,t,&parts,EFS_TXN_ABORT);
            if(aborted!=EFS_OK)rc=aborted;
            else (void)ops->resolve(ctx,t,&parts,EFS_TXN_ABORT);
            goto out;
        }
    }
    rc=ops->decide(ctx,t,&parts,EFS_TXN_COMMIT);
    if(rc==EFS_OK)rc=ops->resolve(ctx,t,&parts,EFS_TXN_COMMIT);
    /* A failed COMMIT acknowledgement is ambiguous, never an ABORT. */
out:
    return rc;
}

static void finish_free(struct finish_capture *c)
{
    for(uint32_t i=0;i<c->count;++i){free(c->images[i].expected);free(c->images[i].replacement);}
    free(c->images);
}
int efs_meta_finish_append(struct efs_kv *kv,efs_ino_t ino,uint64_t offset,
                            int outcome,const struct efs_txid *t,
                            const struct efs_meta_append_ops *ops,void *ctx)
{
    struct finish_capture c={.base=kv};
    const struct efs_kv_ops capture_ops={.get=finish_get,.scan_prefix=finish_scan,.scan_from=finish_scan_from};
    struct efs_kv capture={.ops=&capture_ops,.ctx=&c};
    if(!kv || !ino || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve)
        return EFS_ERR_INVAL;
    int rc=efs_meta_collect_append_completion(&capture,ino,offset,outcome,finish_batch,&c);
    if(rc==EFS_OK && c.mutations)rc=finish_execute(&c,t,ops,ctx);
    finish_free(&c);return rc;
}

/* Eight deletes plus inode/cursor guards fit the resolver's fixed 48-item
 * per-participant atomic batch (each EXCL consumes up to three items). */
#define APPEND_PRUNE_PAGE 8
struct prune_scan {struct finish_capture *capture;unsigned count;int rc;};
static int prune_row(void *user,const uint8_t *key,uint32_t kl,const uint8_t *val,uint32_t vl)
{
    struct prune_scan *s=user;uint32_t slot;
    if(vl!=48 || val[44]!=1){s->rc=EFS_ERR_PROTO;return 1;}
    ++s->count;
    if(!s->capture)return 1;
    s->rc=finish_record(s->capture,key,kl,val,vl,&slot);
    if(s->rc!=EFS_OK)return 1;
    s->capture->images[slot].op=EFS_TXN_DEL;
    s->capture->images[slot].new_len=0;
    return s->count==APPEND_PRUNE_PAGE;
}
static int prune_capture(struct efs_kv *kv,efs_ino_t ino,struct finish_capture *c,unsigned *count)
{
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_OPID_VAL_MAX];uint32_t kl,n;
    struct efs_meta_row row;
    struct prune_scan scan={.capture=c};
    const struct efs_kv_ops capture_ops={.get=finish_get};
    struct efs_kv capture={.ops=&capture_ops,.ctx=c};
    struct efs_kv *reader=c?&capture:kv;
    *count=0;
    int rc=efs_meta_apply_get_inode(reader,ino,&row);
    if(rc!=EFS_OK)return rc;
    rc=efs_kv_key_append_cur(efs_kv_inode_shard(ino),ino,row.generation,key,&kl);
    if(rc!=EFS_OK)return rc;
    n=sizeof(value);rc=efs_kv_get(reader,key,kl,value,&n);
    if(rc==EFS_OK) {
        if(n!=24)return EFS_ERR_PROTO;
        /* DONE entries still carry frontier dependencies while OPEN exists. */
        if(value[16] || value[17] || value[18] || value[19])return EFS_OK;
    } else if(rc!=EFS_ERR_NOT_FOUND)return rc;
    rc=efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino),ino,row.generation,key,&kl);
    if(rc==EFS_OK)rc=efs_kv_scan_prefix(kv,key,kl,prune_row,&scan);
    if(scan.rc!=EFS_OK)return scan.rc;
    *count=scan.count;return rc==1?EFS_OK:rc;
}
int efs_meta_append_prune_pending(struct efs_kv *kv,efs_ino_t ino)
{
    unsigned count; if(!kv || !ino)return EFS_ERR_INVAL;
    int rc=prune_capture(kv,ino,NULL,&count);
    return rc==EFS_OK?(count?1:0):rc;
}
int efs_meta_prune_append(struct efs_kv *kv,efs_ino_t ino,const struct efs_txid *t,
                           const struct efs_meta_append_ops *ops,void *ctx,unsigned *removed)
{
    struct finish_capture c={.base=kv};unsigned count=0;
    if(!kv || !ino || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve || !removed)
        return EFS_ERR_INVAL;
    *removed=0;
    int rc=prune_capture(kv,ino,&c,&count);
    if(rc==EFS_OK && count)rc=finish_execute(&c,t,ops,ctx);
    if(rc==EFS_OK)*removed=count;
    finish_free(&c);return rc;
}

static int close_capture(struct efs_kv *kv,efs_ino_t ino,const uint8_t *identity,
                            struct finish_capture *c)
{
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_CLOSE_STEP_VALUE];uint32_t kl,n=sizeof(value),slot;
    int rc=efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),ino,key,&kl);
    if(rc!=EFS_OK)return rc;
    key[2]=EFS_KV_KIND_CLOSE_STEP;
    rc=efs_kv_get(kv,key,kl,value,&n);
    if(rc==EFS_ERR_NOT_FOUND)return EFS_ERR_STALE;
    if(rc!=EFS_OK)return rc;
    if(n!=sizeof(value))return EFS_ERR_PROTO;
    if(memcmp(value,identity,sizeof(value)))return EFS_ERR_STALE;
    struct efs_meta_row row;
    rc=efs_meta_apply_get_inode(kv,ino,&row);
    if(rc!=EFS_OK)return rc;
    uint64_t gen=0;for(unsigned i=0;i<8;i++)gen=gen<<8|identity[i];
    if(row.generation!=gen)return EFS_ERR_STALE;
    c->close_ino=ino;
    return finish_record(c,key,kl,value,n,&slot);
}
int efs_meta_finish_append_close(struct efs_kv *kv,efs_ino_t ino,uint64_t offset,
    const uint8_t identity[EFS_CLOSE_STEP_VALUE],const struct efs_txid *t,
    const struct efs_meta_append_ops *ops,void *ctx)
{
    struct finish_capture c={.base=kv};
    const struct efs_kv_ops capture_ops={.get=finish_get,.scan_prefix=finish_scan,.scan_from=finish_scan_from};
    struct efs_kv capture={.ops=&capture_ops,.ctx=&c};
    if(!kv || !ino || !identity || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve)
        return EFS_ERR_INVAL;
    int rc=close_capture(kv,ino,identity,&c);
    if(rc==EFS_OK)rc=efs_meta_collect_append_completion(&capture,ino,offset,
        EFS_META_APPEND_ABORTED_HOLE,finish_batch,&c);
    if(rc==EFS_OK && c.mutations)rc=finish_execute(&c,t,ops,ctx);
    finish_free(&c);return rc;
}
int efs_meta_prune_append_close(struct efs_kv *kv,efs_ino_t ino,
    const uint8_t identity[EFS_CLOSE_STEP_VALUE],const struct efs_txid *t,
    const struct efs_meta_append_ops *ops,void *ctx,unsigned *removed)
{
    struct finish_capture c={.base=kv};unsigned count=0;
    if(!kv || !ino || !identity || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve || !removed)
        return EFS_ERR_INVAL;
    *removed=0;
    int rc=close_capture(kv,ino,identity,&c);
    if(rc==EFS_OK)rc=prune_capture(kv,ino,&c,&count);
    if(rc==EFS_OK && count)rc=finish_execute(&c,t,ops,ctx);
    if(rc==EFS_OK)*removed=count;
    finish_free(&c);return rc;
}
