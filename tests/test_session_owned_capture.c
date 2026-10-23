/* K5 / I23 review finding S2: a session-owned executor checks the session
 * (efs_session_accept), then captures the local session row as its EXCL
 * comparison image. A FENCE_LOC landing between those two reads must refuse
 * the request from the captured bytes; the fenced row must never become the
 * CAS base. A fence after the capture must conflict at the owner PREPARE,
 * and one after the PREPARE must wait for the established transaction.
 *
 * The barrier is a KV wrapper around a real session/inode store: the Nth
 * read of the session row runs the fence (or a row removal) on the store
 * first, exactly between the executor's check and its capture. */
#include "efs/meta_apply.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int decision(void *ctx,const struct efs_txid *t,uint32_t sh,int *d)
{ return efs_txn_decision_get(ctx,sh,t,d); }

/* ---- Barrier: injection on the Nth read of the session row ---- */
enum { INJECT_NONE, INJECT_FENCE, INJECT_DELETE };
static struct {
    struct efs_kv *inner;
    struct efs_kv_ops ops;
    struct efs_kv kv;
    int (*real_get)(void *,const uint8_t *,uint32_t,uint8_t *,uint32_t *);
    uint8_t key[EFS_KV_KEY_MAX];uint32_t klen;
    uint32_t shard;uint8_t uuid[EFS_OPID_UUID_LEN];
    unsigned reads,fire_at,mode,fired;
    uint64_t after_injection;
} bar;
static uint64_t fingerprint(struct efs_kv *kv);
static void inject(void)
{
    if(bar.mode==INJECT_FENCE)
        assert(efs_session_fence_local(bar.inner,bar.shard,bar.uuid,2)==EFS_OK);
    else if(bar.mode==INJECT_DELETE)
        assert(efs_kv_del(bar.inner,bar.key,bar.klen)==EFS_OK);
    bar.fired++;bar.after_injection=fingerprint(bar.inner);
}
static int barrier_get(void *ctx,const uint8_t *key,uint32_t n,uint8_t *value,uint32_t *len)
{
    if(n==bar.klen && !memcmp(key,bar.key,n) && ++bar.reads==bar.fire_at)inject();
    return bar.real_get(ctx,key,n,value,len);
}
static struct efs_kv *barrier(struct efs_kv *inner,uint32_t shard,const uint8_t *uuid,
                              unsigned fire_at,unsigned mode)
{
    bar.inner=inner;bar.ops=*inner->ops;bar.real_get=inner->ops->get;
    bar.ops.get=barrier_get;bar.ops.destroy=NULL;
    bar.kv.ops=&bar.ops;bar.kv.ctx=inner->ctx;
    bar.shard=shard;memcpy(bar.uuid,uuid,EFS_OPID_UUID_LEN);
    assert(efs_kv_key_sess_local(shard,uuid,bar.key,&bar.klen)==EFS_OK);
    bar.reads=0;bar.fire_at=fire_at;bar.mode=mode;bar.fired=0;
    return &bar.kv;
}

/* Inode, lane, append and transaction-intent state; a refused owner must
 * leave all of it exactly as the fence left it. */
struct fp { uint64_t h; };
static int fp_cb(void *user,const uint8_t *key,uint32_t klen,const uint8_t *value,uint32_t vlen)
{
    struct fp *f=user;
    if(klen<3)return 0;
    switch(key[2]) {
    case EFS_KV_KIND_INODE: case EFS_KV_KIND_LANE: case EFS_KV_KIND_INTENT:
    case EFS_KV_KIND_GUARD: case EFS_KV_KIND_REDUCE: case EFS_KV_KIND_APPEND_CUR:
    case EFS_KV_KIND_APPEND_RSV: case EFS_KV_KIND_APPEND_RECEIPT: case EFS_KV_KIND_SESS_LOCAL:
        break;
    default: return 0;
    }
    for(uint32_t i=0;i<klen;i++)f->h=(f->h^key[i])*1099511628211ull;
    f->h=(f->h^0xff)*1099511628211ull;
    for(uint32_t i=0;i<vlen;i++)f->h=(f->h^value[i])*1099511628211ull;
    f->h=(f->h^0xfe)*1099511628211ull;
    return 0;
}
static uint64_t fingerprint(struct efs_kv *kv)
{struct fp f={1469598103934665603ull};assert(efs_kv_scan(kv,fp_cb,&f)==EFS_OK);return f.h;}

/* ---- Transaction callbacks with spies ---- */
enum { SPY_PLAIN, SPY_FENCE_BEFORE_OWNER, SPY_FENCE_AFTER_OWNER };
static unsigned spy_mode,prepares,owner_prepares;
static uint32_t spy_shard;static uint8_t spy_uuid[EFS_OPID_UUID_LEN];
static struct efs_txn_parts last_parts;
static int is_owner(const uint8_t *key,uint32_t n){return n>=3 && key[2]==EFS_KV_KIND_SESS_LOCAL;}
static int mtime_prepare(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,
                         const struct efs_meta_mtime_image *q)
{
    struct efs_kv *kv=ctx;int owner=is_owner(q->key,q->klen);
    prepares++;owner_prepares+=owner;last_parts=*p;
    if(owner && spy_mode==SPY_FENCE_BEFORE_OWNER) {
        assert(efs_session_fence_local(kv,spy_shard,spy_uuid,2)==EFS_OK);
        bar.after_injection=fingerprint(kv);
    }
    int rc=efs_txn_prepare_excl_value(kv,t,p,q->key,q->klen,q->expected,q->expected_len,
                                      EFS_TXN_PUT,q->value,q->new_len);
    if(owner && rc==EFS_OK && spy_mode==SPY_FENCE_AFTER_OWNER)
        assert(efs_session_fence_local(kv,spy_shard,spy_uuid,2)==EFS_ERR_BUSY);
    return rc;
}
static int append_prepare(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,
                          const struct efs_meta_append_image *q)
{
    struct efs_kv *kv=ctx;prepares++;owner_prepares+=is_owner(q->key,q->klen);last_parts=*p;
    return efs_txn_prepare_excl_value(kv,t,p,q->key,q->klen,q->expected,q->expected_len,
                                      q->op,q->value,q->new_len);
}
static int decide(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{ return efs_txn_decide(ctx,efs_txn_coordinator(t,p),t,d); }
static int resolve(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{
    for(unsigned i=0;i<p->n;++i){int rc=efs_txn_resolve(ctx,t,p->shard[i],d);if(rc)return rc;}
    return EFS_OK;
}
static const struct efs_meta_mtime_ops mtime_ops={mtime_prepare,decide,resolve};
static const struct efs_meta_append_ops append_ops={append_prepare,decide,resolve};

/* ---- Fixture: a regular file with two active lanes and a session at
 * epoch 1 created, registered and established on the inode shard. ---- */
static struct efs_meta_pub publication(efs_ino_t ino,uint64_t gen,unsigned ci,uint64_t end,unsigned candidate)
{
    struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.chunk_index=ci;
    p.new_size=end;p.now=100;p.candidate_gen=candidate;
    p.coding_profile_id=EFS_META_PROFILE_K2F1;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;++i)p.ch.nodes[i]=i+1;
    return p;
}
static struct efs_opid_req identity(void)
{struct efs_opid_req q={0};q.id.client_uuid[0]=42;q.id.session_epoch=1;q.id.seq=1;return q;}
static struct efs_kv *fixture(efs_ino_t *ino,uint32_t *shard)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);
    struct efs_meta_attrs a={1,1,1};struct efs_meta_row row;
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&a,EFS_ROOT_INO,S_IFREG|0644,"file",ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,*ino,&row)==EFS_OK);
    assert(efs_meta_apply_activate_lanes(kv,*ino,3)==EFS_OK);
    struct efs_meta_pub p=publication(*ino,row.generation,0,100,10);
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    p=publication(*ino,row.generation,1,200,20);assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    struct efs_opid_req q=identity();*shard=efs_kv_inode_shard(*ino);
    assert(efs_session_create(kv,q.id.client_uuid,1)==EFS_OK);
    assert(efs_session_register(kv,q.id.client_uuid,1,*shard)==EFS_OK);
    assert(efs_session_establish(kv,*shard,q.id.client_uuid,1)==EFS_OK);
    spy_shard=*shard;memcpy(spy_uuid,q.id.client_uuid,EFS_OPID_UUID_LEN);
    spy_mode=SPY_PLAIN;prepares=owner_prepares=0;
    return kv;
}
static uint64_t mtime_of(struct efs_kv *kv,efs_ino_t ino)
{struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK);return st.mtime;}

/* Fence (or row removal) between accept and capture: refuse from the
 * captured image, prepare nothing, change nothing. */
static void mtime_between(unsigned mode,int expect)
{
    efs_ino_t ino;uint32_t shard;struct efs_kv *kv=fixture(&ino,&shard);
    struct efs_opid_req q=identity();
    struct efs_meta_mtime_plan *plan=malloc(sizeof(*plan));assert(plan);
    assert(efs_meta_capture_mtime(kv,ino,600,EFS_META_SET_MTIME,10,0,decision,kv,plan)==EFS_OK);
    uint64_t before=mtime_of(kv,ino);
    struct efs_kv *w=barrier(kv,shard,q.id.client_uuid,2,mode); /* read 1: accept, 2: capture */
    struct efs_txid t={{31,(uint8_t)mode}};
    int rc=efs_meta_execute_mtime_owned(w,plan,&q.id,&t,&mtime_ops,kv);
    if(rc!=expect)fprintf(stderr,"mtime between mode=%u rc=%d want %d\n",mode,rc,expect);
    assert(bar.fired==1 && rc==expect && !prepares);
    assert(fingerprint(kv)==bar.after_injection && mtime_of(kv,ino)==before);
    if(mode==INJECT_FENCE) /* no owner intent holds the row: a later fence proceeds */
        assert(efs_session_fence_local(kv,shard,q.id.client_uuid,3)==EFS_OK);
    free(plan);efs_kv_mem_free(kv);
}

/* Fence after capture, before the owner PREPARE: the exact-image CAS
 * conflicts; nothing else is prepared and the inode/lanes are unchanged.
 * The retry is refused by the session check. */
static void mtime_after_capture(void)
{
    efs_ino_t ino;uint32_t shard;struct efs_kv *kv=fixture(&ino,&shard);
    struct efs_opid_req q=identity();
    struct efs_meta_mtime_plan *plan=malloc(sizeof(*plan));assert(plan);
    assert(efs_meta_capture_mtime(kv,ino,600,EFS_META_SET_MTIME,10,0,decision,kv,plan)==EFS_OK);
    uint64_t before=mtime_of(kv,ino);
    spy_mode=SPY_FENCE_BEFORE_OWNER;struct efs_txid t={{32}};
    int rc=efs_meta_execute_mtime_owned(kv,plan,&q.id,&t,&mtime_ops,kv);
    assert(rc==EFS_ERR_STALE && prepares==1 && owner_prepares==1);
    assert(fingerprint(kv)==bar.after_injection && mtime_of(kv,ino)==before);
    spy_mode=SPY_PLAIN;prepares=0;t.bytes[1]=1;
    assert(efs_meta_execute_mtime_owned(kv,plan,&q.id,&t,&mtime_ops,kv)==EFS_ERR_FENCED && !prepares);
    free(plan);efs_kv_mem_free(kv);
}

/* Fence after the owner PREPARE: the fence waits (BUSY) behind the intent,
 * the established transaction commits, then the fence lands and the next
 * request of the old epoch is refused. */
static void mtime_after_prepare(void)
{
    efs_ino_t ino;uint32_t shard;struct efs_kv *kv=fixture(&ino,&shard);
    struct efs_opid_req q=identity();
    struct efs_meta_mtime_plan *plan=malloc(sizeof(*plan));assert(plan);
    assert(efs_meta_capture_mtime(kv,ino,600,EFS_META_SET_MTIME,10,0,decision,kv,plan)==EFS_OK);
    spy_mode=SPY_FENCE_AFTER_OWNER;struct efs_txid t={{33}};
    assert(efs_meta_execute_mtime_owned(kv,plan,&q.id,&t,&mtime_ops,kv)==EFS_OK);
    assert(owner_prepares==1 && prepares==plan->count+1 && mtime_of(kv,ino)==10);
    assert(efs_session_fence_local(kv,shard,q.id.client_uuid,2)==EFS_OK);
    spy_mode=SPY_PLAIN;prepares=0;
    assert(efs_meta_capture_mtime(kv,ino,700,EFS_META_SET_MTIME,20,0,decision,kv,plan)==EFS_OK);
    t.bytes[1]=1;
    assert(efs_meta_execute_mtime_owned(kv,plan,&q.id,&t,&mtime_ops,kv)==EFS_ERR_FENCED && !prepares);
    assert(mtime_of(kv,ino)==10);
    free(plan);efs_kv_mem_free(kv);
}

/* The same interleaving against the APPEND reservation executor. */
static void append_between(unsigned mode,int expect)
{
    efs_ino_t ino;uint32_t shard;struct efs_kv *kv=fixture(&ino,&shard);
    struct efs_opid_req q=identity();
    struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
    assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,plan)==EFS_OK);
    struct efs_kv *w=barrier(kv,shard,q.id.client_uuid,2,mode);
    struct efs_txid t={{34,(uint8_t)mode}};
    int rc=efs_meta_execute_append_owned(w,plan,&q,&t,&append_ops,kv);
    if(rc!=expect)fprintf(stderr,"append between mode=%u rc=%d want %d\n",mode,rc,expect);
    assert(bar.fired==1 && rc==expect && !prepares && fingerprint(kv)==bar.after_injection);
    uint32_t open=99;assert(efs_meta_apply_append_state(kv,ino,NULL,NULL,&open)==EFS_OK && !open);
    free(plan);efs_kv_mem_free(kv);
}

/* ... and against APPEND completion (REPORT of a reserved range). */
static void completion_between(unsigned mode,int expect)
{
    efs_ino_t ino;uint32_t shard;struct efs_kv *kv=fixture(&ino,&shard);
    struct efs_opid_req q=identity();
    struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
    assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,plan)==EFS_OK);
    struct efs_txid reserve={{35,(uint8_t)mode}};
    assert(efs_meta_execute_append(plan,&reserve,&append_ops,kv)==EFS_OK);
    prepares=owner_prepares=0;
    struct efs_kv *w=barrier(kv,shard,q.id.client_uuid,2,mode);
    struct efs_txid t={{36,(uint8_t)mode}};
    int rc=efs_meta_finish_append_owned(w,ino,plan->offset,EFS_META_APPEND_COMPLETED,&q.id,&t,&append_ops,kv);
    if(rc!=expect)fprintf(stderr,"completion between mode=%u rc=%d want %d\n",mode,rc,expect);
    assert(bar.fired==1 && rc==expect && !prepares && fingerprint(kv)==bar.after_injection);
    uint32_t open=0;assert(efs_meta_apply_append_state(kv,ino,NULL,NULL,&open)==EFS_OK && open==1);
    free(plan);efs_kv_mem_free(kv);
}

/* efs_session_prepare_owner: its EXCL image is its own second read. */
static void prepare_owner_between(unsigned mode,int expect)
{
    efs_ino_t ino;uint32_t shard;struct efs_kv *kv=fixture(&ino,&shard);
    struct efs_opid_req q=identity();
    struct efs_txn_parts parts={0};parts.shard[parts.n++]=shard;
    struct efs_txid t={{37,(uint8_t)mode}};
    struct efs_kv *w=barrier(kv,shard,q.id.client_uuid,2,mode);
    int rc=efs_session_prepare_owner(w,shard,q.id.client_uuid,1,&t,&parts);
    if(rc!=expect)fprintf(stderr,"prepare owner between mode=%u rc=%d want %d\n",mode,rc,expect);
    assert(bar.fired==1 && rc==expect && fingerprint(kv)==bar.after_injection);
    efs_kv_mem_free(kv);
}

int main(void)
{
    mtime_between(INJECT_FENCE,EFS_ERR_FENCED);
    mtime_between(INJECT_DELETE,EFS_ERR_UNREGISTERED);
    mtime_after_capture();
    mtime_after_prepare();
    append_between(INJECT_FENCE,EFS_ERR_FENCED);
    append_between(INJECT_DELETE,EFS_ERR_UNREGISTERED);
    completion_between(INJECT_FENCE,EFS_ERR_FENCED);
    completion_between(INJECT_DELETE,EFS_ERR_UNREGISTERED);
    prepare_owner_between(INJECT_FENCE,EFS_ERR_FENCED);
    prepare_owner_between(INJECT_DELETE,EFS_ERR_UNREGISTERED);
    puts("session-owned capture: fence/removal between check and capture refuses with no "
         "inode, lane, append or intent change; fence after capture conflicts; fence after "
         "owner PREPARE waits for the established transaction PASS");
    return 0;
}
