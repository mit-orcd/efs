#include "efs/repair_job.h"
#include "efs/checksum.h"
#include "efs/metadata.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include <stdlib.h>
#include <string.h>

static void w32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static void w64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static uint32_t r32(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static uint64_t r64(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static int valid(const struct efs_repair_job *j)
{
    uint8_t any=0,store=0;
    if(!j)return EFS_ERR_INVAL;
    for(unsigned i=0;i<16;i++){any|=j->uuid[i];store|=j->target_store[i];}
    if(!any || !store || j->ino<=EFS_ROOT_INO || efs_ino_is_meta_table(j->ino) || !j->inode_gen ||
       !j->object || !j->epoch || j->epoch==UINT32_MAX || !j->owner || j->fi>=3 ||
       !efs_chunk_size_valid(j->fragment_len*2u) || j->fragment_len>EFS_MAX_FRAGMENT_SIZE ||
       (!j->span_len && j->span_off) || (uint64_t)j->span_off+j->span_len>2ull*j->fragment_len ||
       j->state<1 || j->state>3 || (j->flags&~3u) ||
       (j->state==EFS_REPAIR_CANCELLING && !(j->flags&EFS_REPAIR_CANCELLED)) ||
       (j->state==EFS_REPAIR_ADMITTED && (j->flags&EFS_REPAIR_CANCELLED)) ||
       (j->state==EFS_REPAIR_SEALED && !j->flags))return EFS_ERR_INVAL;
    for(unsigned i=0;i<3;i++){
        if(!j->nodes[i])return EFS_ERR_INVAL;
        for(unsigned k=0;k<i;k++)if(j->nodes[k]==j->nodes[i])return EFS_ERR_INVAL;
    }
    return EFS_OK;
}
int efs_repair_job_encode(const struct efs_repair_job *j,uint8_t out[EFS_REPAIR_JOB_BYTES])
{
    if(!out || valid(j))return EFS_ERR_INVAL;
    memset(out,0,EFS_REPAIR_JOB_BYTES);memcpy(out,"EFR2",4);memcpy(out+4,j->uuid,16);
    w64(out+20,j->ino);w64(out+28,j->inode_gen);w64(out+36,j->object);w32(out+44,j->ci);
    w32(out+48,j->fragment_len);w32(out+52,j->span_off);w32(out+56,j->span_len);
    w32(out+60,j->epoch);w32(out+64,j->owner);
    for(unsigned i=0;i<3;i++)w32(out+68+4*i,j->nodes[i]);
    memcpy(out+80,j->sums,96);memcpy(out+176,j->source,32);
    out[208]=j->fi;out[209]=j->state;out[210]=j->flags;
    memcpy(out+212,j->target_store,16);efs_hash(out,228,out+228);return EFS_OK;
}
int efs_repair_job_decode(const uint8_t *in,uint32_t n,struct efs_repair_job *out)
{
    uint8_t h[32];struct efs_repair_job j={0};
    if(!in || !out || n!=EFS_REPAIR_JOB_BYTES || memcmp(in,"EFR2",4) || in[211])return EFS_ERR_PROTO;
    efs_hash(in,228,h);if(memcmp(h,in+228,32))return EFS_ERR_PROTO;
    memcpy(j.uuid,in+4,16);j.ino=r64(in+20);j.inode_gen=r64(in+28);j.object=r64(in+36);
    j.ci=r32(in+44);j.fragment_len=r32(in+48);j.span_off=r32(in+52);j.span_len=r32(in+56);
    j.epoch=r32(in+60);j.owner=r32(in+64);for(unsigned i=0;i<3;i++)j.nodes[i]=r32(in+68+4*i);
    memcpy(j.sums,in+80,96);memcpy(j.source,in+176,32);j.fi=in[208];j.state=in[209];j.flags=in[210];memcpy(j.target_store,in+212,16);
    if(valid(&j))return EFS_ERR_PROTO;
    *out=j;return EFS_OK;
}
static uint32_t anchor(const struct efs_repair_job *j){return efs_kv_anchor_shard(efs_kv_lane_shard(j->ino,j->ci%64));}
static void key(const struct efs_repair_job *j,uint8_t kind,uint8_t out[19])
{uint32_t a=anchor(j);out[0]=(uint8_t)(a>>8);out[1]=(uint8_t)a;out[2]=kind;memcpy(out+3,j->uuid,16);}
/* Hash canonical bytes, never C padding; the complete chunk row, lane stamp
 * and lane fence history bind both publication and truncation changes. */
static int source_hash(struct efs_kv *kv,const struct efs_repair_job *j,uint8_t out[32])
{
    uint8_t keys[3][EFS_KV_KEY_MAX];uint32_t lens[3]={0};
    uint32_t sh=efs_kv_lane_shard(j->ino,j->ci%64);
    int rc=efs_kv_key_chunk(sh,j->ino,j->inode_gen,j->ci%64,j->ci,keys[0],&lens[0]);
    if(!rc)rc=efs_kv_key_lane(sh,j->ino,j->inode_gen,j->ci%64,keys[1],&lens[1]);
    if(rc)return rc;
    memcpy(keys[2],keys[1],lens[1]);lens[2]=lens[1];keys[2][2]=EFS_KV_KIND_CONTENT_FENCE;
    uint8_t bytes[8192];uint32_t used=0;
    for(unsigned i=0;i<3;i++){
        rc=efs_txn_key_exclusive(kv,keys[i],lens[i]);if(rc)return rc;
        /* Reserve key framing and value length before any append/subtraction. */
        if(lens[i]>EFS_KV_KEY_MAX || used>sizeof(bytes) ||
           sizeof(bytes)-used<8u || lens[i]>sizeof(bytes)-used-8u)
            return EFS_ERR_OVERFLOW;
        w32(bytes+used,lens[i]);used+=4;memcpy(bytes+used,keys[i],lens[i]);used+=lens[i];
        uint32_t n=sizeof(bytes)-used-4;
        rc=efs_kv_get(kv,keys[i],lens[i],bytes+used+4,&n);
        if(rc==EFS_ERR_NOT_FOUND && i==2){rc=EFS_OK;n=0;}
        if(rc)return rc;
        if(n>sizeof(bytes)-used-4u)return EFS_ERR_OVERFLOW;
        w32(bytes+used,n);used+=4+n;
    }
    efs_hash(bytes,used,out);return EFS_OK;
}
int efs_repair_capture(struct efs_kv *kv,const struct efs_repair_job *sel,struct efs_repair_job *out)
{
    struct efs_meta_chunk_view view;struct efs_meta_chunk ch;uint32_t n=0;
    struct efs_repair_job j;
    if(!kv || !sel || !out)return EFS_ERR_INVAL;
    j=*sel;j.state=EFS_REPAIR_ADMITTED;j.flags=0;
    /* Selector validation does not require its yet-to-be-filled placement. */
    for(unsigned i=0;i<3;i++)j.nodes[i]=i+1;
    int rc=valid(&j);if(rc)return rc;
    rc=efs_meta_get_chunk_view(kv,j.ino,j.inode_gen,j.ci,j.fragment_len*2u,&view);
    if(rc)return rc;
    ch=view.base;n=view.ndelta;const struct efs_meta_delta *ds=view.deltas;
    if(n>EFS_CHUNK_DELTA_MAX || ch.packed.segment_id || ch.coding_profile_id!=EFS_META_PROFILE_K2F1)return EFS_ERR_INVAL;
    int found=0;
    if(!j.span_len && ch.generation==j.object && view.bytes.parts[0].len){memcpy(j.nodes,ch.nodes,sizeof(j.nodes));memcpy(j.sums,ch.checksums,sizeof(j.sums));found=1;}
    for(uint32_t i=0;i<n && !found;i++)if(j.span_len && ds[i].generation==j.object && ds[i].off==j.span_off && ds[i].len==j.span_len && view.bytes.parts[i+1].len){
        memcpy(j.nodes,ds[i].nodes,sizeof(j.nodes));memcpy(j.sums,ds[i].checksums,sizeof(j.sums));found=1;
    }
    if(!found)return EFS_ERR_STALE;
    for(unsigned f=0;f<3;f++){uint8_t any=0;for(unsigned k=0;k<32;k++)any|=j.sums[f][k];if(!any)return EFS_ERR_INVAL;}
    rc=valid(&j);if(!rc)rc=source_hash(kv,&j,j.source);
    if(!rc)*out=j;
    return rc;
}
static int same(const struct efs_repair_job *a,const struct efs_repair_job *b)
{
    uint8_t x[EFS_REPAIR_JOB_BYTES],y[EFS_REPAIR_JOB_BYTES];struct efs_repair_job aa=*a,bb=*b;
    aa.state=bb.state=EFS_REPAIR_ADMITTED;aa.flags=bb.flags=0;
    return !efs_repair_job_encode(&aa,x) && !efs_repair_job_encode(&bb,y) && !memcmp(x,y,sizeof(x));
}
static void digest(const struct efs_repair_job *j,uint8_t out[32])
{struct efs_repair_job c=*j;uint8_t b[EFS_REPAIR_JOB_BYTES];c.state=1;c.flags=0;efs_repair_job_encode(&c,b);efs_hash(b,sizeof(b),out);}
int efs_repair_get(struct efs_kv *kv,const struct efs_repair_job *j,struct efs_repair_job *out)
{
    uint8_t k[19],v[EFS_REPAIR_JOB_BYTES];uint32_t n=sizeof(v);if(valid(j) || !out)return EFS_ERR_INVAL;
    key(j,EFS_KV_KIND_REPAIR_JOB,k);int rc=efs_kv_get(kv,k,sizeof(k),v,&n);if(rc)return rc;
    rc=efs_repair_job_decode(v,n,out);if(!rc && !same(j,out))rc=EFS_ERR_STALE;return rc;
}
static int put(struct efs_kv *kv,const struct efs_repair_job *j)
{
    uint8_t k[19],v[EFS_REPAIR_JOB_BYTES];int rc=efs_repair_job_encode(j,v);if(rc)return rc;
    key(j,EFS_KV_KIND_REPAIR_JOB,k);struct efs_kv_item item={EFS_KV_PUT,k,sizeof(k),v,sizeof(v)};
    return efs_kv_batch(kv,&item,1);
}
struct scan {const struct efs_repair_job *match;uint32_t count;int duplicate,rc,any_gen,any_fi;};
static int scan_cb(void *u,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl)
{
    struct scan *s=u;struct efs_repair_job j;uint8_t expected[19];
    if(kl!=19 || efs_repair_job_decode(v,vl,&j)){s->rc=EFS_ERR_PROTO;return 1;}
    key(&j,EFS_KV_KIND_REPAIR_JOB,expected);
    if(memcmp(k,expected,19)){s->rc=EFS_ERR_PROTO;return 1;}
    if(++s->count>EFS_REPAIR_PENDING_MAX){s->rc=EFS_ERR_OVERFLOW;return 1;}
    const struct efs_repair_job *m=s->match;
    if(m && j.ino==m->ino && (s->any_gen || j.inode_gen==m->inode_gen) && j.ci==m->ci && j.object==m->object && (s->any_fi || j.fi==m->fi)){
        s->duplicate=1;
        if(j.state!=EFS_REPAIR_SEALED)s->rc=EFS_ERR_BUSY;
    }
    return 0;
}
static int scan_jobs(struct efs_kv *kv,const struct efs_repair_job *j,struct scan *s)
{
    uint8_t k[19];key(j,EFS_KV_KIND_REPAIR_JOB,k);
    int rc=efs_kv_scan_prefix(kv,k,3,scan_cb,s);return s->rc?s->rc:(rc==1?EFS_OK:rc);
}
static int gc_not_scheduled(struct efs_kv *kv,const struct efs_repair_job *j)
{
    /* ADMIT and GC scheduling apply on the same lane group. Even if a
     * historical row remains visible, a committed GC record forbids new jobs.
     * Check under the outer atomic apply scope, before recording authority. */
    uint8_t gk[EFS_KV_KEY_MAX],gv[EFS_META_GC_VAL];uint32_t gkl=0,gn=sizeof(gv);
    int rc=efs_kv_key_gc(anchor(j),j->ino,j->object,j->ci%64,j->ci,gk,&gkl);
    if(rc)return rc;
    rc=efs_txn_key_exclusive(kv,gk,gkl);if(rc)return rc;
    rc=efs_kv_get(kv,gk,gkl,gv,&gn);
    if(rc==EFS_OK || (rc==EFS_ERR_INVAL && gn>sizeof(gv)))return EFS_ERR_STALE;
    if(rc!=EFS_ERR_NOT_FOUND)return rc;
    return EFS_OK;
}
/* ReadIndex must precede this read; current source and GC scheduling are part
 * of the permit, not just an old ADMITTED journal flag. FileID/export authority
 * still requires the receiver's separate inode writer view and geometry check. */
int efs_repair_write_authority(struct efs_kv *kv,const struct efs_repair_job *j)
{
    if(valid(j) || j->state!=EFS_REPAIR_ADMITTED || j->flags)return EFS_ERR_INVAL;
    struct efs_repair_job old,now;int rc=efs_repair_get(kv,j,&old);
    if(rc)return rc;
    if(old.state!=EFS_REPAIR_ADMITTED || (old.flags&EFS_REPAIR_CANCELLED))return EFS_ERR_STALE;
    rc=gc_not_scheduled(kv,j);if(rc)return rc;
    rc=efs_repair_capture(kv,j,&now);
    if(rc)return rc==EFS_ERR_NOT_FOUND?EFS_ERR_STALE:rc;
    return same(j,&now)?EFS_OK:EFS_ERR_STALE;
}
int efs_repair_admit(struct efs_kv *kv,const struct efs_repair_job *j)
{
    if(valid(j) || j->state!=EFS_REPAIR_ADMITTED || j->flags)return EFS_ERR_INVAL;
    struct efs_repair_job old,now;int rc=efs_repair_get(kv,j,&old);
    if(rc!=EFS_ERR_NOT_FOUND)return rc; /* identical replay succeeds, conflict does not */
    uint8_t fk[19],fv[76],h[32];uint32_t fn=sizeof(fv);key(j,EFS_KV_KIND_REPAIR_FLOOR,fk);
    rc=efs_kv_get(kv,fk,sizeof(fk),fv,&fn);
    if(rc==EFS_OK){
        if(fn!=76 || memcmp(fv,"ERF1",4) || r32(fv+4)<2 || fv[40]>1 || fv[41] || fv[42] || fv[43])return EFS_ERR_PROTO;
        efs_hash(fv,44,h);if(memcmp(h,fv+44,32))return EFS_ERR_PROTO;
        return EFS_ERR_STALE; /* A terminal UUID is never re-admitted. */
    }
    if(rc!=EFS_ERR_NOT_FOUND)return rc;
    rc=gc_not_scheduled(kv,j);if(rc)return rc;
    rc=efs_repair_capture(kv,j,&now);if(rc)return rc==EFS_ERR_NOT_FOUND?EFS_ERR_STALE:rc;if(!same(j,&now))return EFS_ERR_STALE;
    struct scan s={.match=j};rc=scan_jobs(kv,j,&s);if(rc && rc!=EFS_ERR_BUSY)return rc;
    if(s.duplicate || s.count>=EFS_REPAIR_PENDING_MAX)return EFS_ERR_BUSY;
    return put(kv,j);
}
static int load(struct efs_kv *kv,const struct efs_repair_job *j,struct efs_repair_job *out)
{return efs_repair_get(kv,j,out);}
int efs_repair_record_data(struct efs_kv *kv,const struct efs_repair_job *j)
{
    struct efs_repair_job old,now;int rc=load(kv,j,&old);if(rc)return rc;
    if(old.state!=EFS_REPAIR_ADMITTED)return EFS_ERR_STALE;
    rc=efs_repair_capture(kv,j,&now);if(rc)return rc==EFS_ERR_NOT_FOUND?EFS_ERR_STALE:rc;if(!same(j,&now))return EFS_ERR_STALE;
    if(old.flags&EFS_REPAIR_DATA_DURABLE)return EFS_OK;
    old.flags|=EFS_REPAIR_DATA_DURABLE;return put(kv,&old);
}
int efs_repair_cancel(struct efs_kv *kv,const struct efs_repair_job *j)
{
    struct efs_repair_job old;int rc=load(kv,j,&old);if(rc)return rc;
    if(old.flags&EFS_REPAIR_CANCELLED)return EFS_OK;
    old.flags|=EFS_REPAIR_CANCELLED;
    if(old.state!=EFS_REPAIR_SEALED)old.state=EFS_REPAIR_CANCELLING;
    return put(kv,&old);
}
int efs_repair_record_seal(struct efs_kv *kv,const struct efs_repair_job *j,uint32_t floor)
{
    struct efs_repair_job old;int rc=load(kv,j,&old);if(rc)return rc;
    if(floor!=j->epoch+1u)return EFS_ERR_STALE;
    if(!(old.flags&(EFS_REPAIR_DATA_DURABLE|EFS_REPAIR_CANCELLED)))return EFS_ERR_BUSY;
    if(old.state==EFS_REPAIR_SEALED)return EFS_OK;
    old.state=EFS_REPAIR_SEALED;return put(kv,&old);
}
int efs_repair_retire(struct efs_kv *kv,const struct efs_repair_job *j,int cancelled)
{
    struct efs_repair_job old,now;int rc=load(kv,j,&old);
    if(rc==EFS_ERR_NOT_FOUND){
        uint8_t fk[19],v[76],h[32],want[32];uint32_t n=sizeof(v);
        key(j,EFS_KV_KIND_REPAIR_FLOOR,fk);rc=efs_kv_get(kv,fk,sizeof(fk),v,&n);
        if(rc)return rc;
        if(n!=sizeof(v) || memcmp(v,"ERF1",4) || v[40]>1 || v[41] || v[42] || v[43])return EFS_ERR_PROTO;
        efs_hash(v,44,h);if(memcmp(h,v+44,32))return EFS_ERR_PROTO;
        digest(j,want);
        return r32(v+4)==j->epoch+1u && !memcmp(want,v+8,32) && v[40]==!!cancelled?EFS_OK:EFS_ERR_STALE;
    }
    if(rc)return rc;
    if(old.state!=EFS_REPAIR_SEALED || !!(old.flags&EFS_REPAIR_CANCELLED)!=!!cancelled)return EFS_ERR_BUSY;
    if(!cancelled){
        if(!(old.flags&EFS_REPAIR_DATA_DURABLE))return EFS_ERR_BUSY;
        rc=efs_repair_capture(kv,j,&now);if(rc)return rc==EFS_ERR_NOT_FOUND?EFS_ERR_STALE:rc;if(!same(j,&now))return EFS_ERR_STALE;
    }
    uint8_t k[19],fk[19],v[76]={0};key(j,EFS_KV_KIND_REPAIR_JOB,k);key(j,EFS_KV_KIND_REPAIR_FLOOR,fk);
    memcpy(v,"ERF1",4);w32(v+4,j->epoch+1u);digest(j,v+8);v[40]=!!cancelled;efs_hash(v,44,v+44);
    struct efs_kv_item items[2]={{EFS_KV_DEL,k,sizeof(k),NULL,0},{EFS_KV_PUT,fk,sizeof(fk),v,sizeof(v)}};
    return efs_kv_batch(kv,items,2);
}
int efs_repair_gc_ready(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint32_t ci,uint64_t object)
{
    struct efs_repair_job j={.ino=ino,.inode_gen=gen,.ci=ci,.object=object};struct scan s={.match=&j};
    if(!kv || !ino || !gen || !object)return EFS_ERR_INVAL;
    s.any_fi=1;return scan_jobs(kv,&j,&s);
}

/* GC keys carry object generation, not FileID generation. Fence ALL matching
 * historical identities rather than accidentally skipping an older job. */
int efs_repair_gc_object_ready(struct efs_kv *kv,efs_ino_t ino,uint32_t ci,uint64_t object)
{
    if(!kv || ino<=EFS_ROOT_INO || !object)return EFS_ERR_INVAL;
    struct efs_repair_job j={.ino=ino,.ci=ci,.object=object};
    struct scan s={.match=&j,.any_gen=1,.any_fi=1};
    return scan_jobs(kv,&j,&s);
}

int efs_repair_apply_command(struct efs_kv *kv,uint32_t group,const uint8_t *cmd,uint32_t n)
{
    struct efs_repair_job j;
    if(!kv || !cmd || n!=EFS_REPAIR_COMMAND_BYTES || cmd[0]!=EFS_MD_CMD_REPAIR_JOB)return EFS_ERR_PROTO;
    int rc=efs_repair_job_decode(cmd+2,EFS_REPAIR_JOB_BYTES,&j);if(rc)return rc;
    if(j.state!=EFS_REPAIR_ADMITTED || j.flags || group!=efs_raft_shard_group(efs_kv_lane_shard(j.ino,j.ci%64)))return EFS_ERR_INVAL;
    uint32_t arg=r32(cmd+2+EFS_REPAIR_JOB_BYTES);
    switch(cmd[1]){
    case EFS_REPAIR_OP_ADMIT:return arg?EFS_ERR_INVAL:efs_repair_admit(kv,&j);
    case EFS_REPAIR_OP_DATA:return arg?EFS_ERR_INVAL:efs_repair_record_data(kv,&j);
    case EFS_REPAIR_OP_CANCEL:return arg?EFS_ERR_INVAL:efs_repair_cancel(kv,&j);
    case EFS_REPAIR_OP_SEAL:return efs_repair_record_seal(kv,&j,arg);
    case EFS_REPAIR_OP_RETIRE:return arg>1?EFS_ERR_INVAL:efs_repair_retire(kv,&j,arg);
    default:return EFS_ERR_PROTO;
    }
}

/* Every replica validates the committed entry term, not its current leader's
 * term. Old committed entries remain replayable after an election. Cancel and
 * terminal cleanup may deliberately operate on a previous owner's job. */
int efs_repair_apply_command_term(struct efs_kv *kv,uint32_t group,uint64_t term,
                                 const uint8_t *cmd,uint32_t n)
{
    if(!cmd || n!=EFS_REPAIR_COMMAND_BYTES)return EFS_ERR_PROTO;
    if(cmd[1]==EFS_REPAIR_OP_ADMIT || cmd[1]==EFS_REPAIR_OP_DATA){
        struct efs_repair_job j;int rc=efs_repair_job_decode(cmd+2,EFS_REPAIR_JOB_BYTES,&j);
        if(rc)return rc;
        if(!term || term>UINT32_MAX-1ull || term!=j.epoch)return EFS_ERR_STALE;
    }
    return efs_repair_apply_command(kv,group,cmd,n);
}

/* Receipt encoding is not authority. Only the trusted coordinator may record
 * it, after receiving it from the exact target's completed storage operation. */
int efs_repair_receipt_encode(const struct efs_repair_job *j,unsigned kind,
                              uint8_t out[EFS_REPAIR_RECEIPT_BYTES])
{
    if(!out || valid(j) || (kind!=EFS_REPAIR_RECEIPT_DATA && kind!=EFS_REPAIR_RECEIPT_SEAL))return EFS_ERR_INVAL;
    memset(out,0,EFS_REPAIR_RECEIPT_BYTES);memcpy(out,kind==EFS_REPAIR_RECEIPT_DATA?"ERD1":"ERS1",4);
    digest(j,out+4);memcpy(out+36,j->target_store,16);
    w32(out+52,j->epoch+(kind==EFS_REPAIR_RECEIPT_SEAL));w32(out+56,j->nodes[j->fi]);
    w32(out+60,j->fi);w32(out+64,kind==EFS_REPAIR_RECEIPT_DATA?j->fragment_len:0);
    memcpy(out+68,j->sums[j->fi],EFS_HASH_SIZE);efs_hash(out,100,out+100);return EFS_OK;
}
int efs_repair_receipt_verify(const struct efs_repair_job *j,unsigned kind,
                              const uint8_t *receipt,uint32_t n)
{
    uint8_t expected[EFS_REPAIR_RECEIPT_BYTES],sum[32];
    if(!receipt || n!=EFS_REPAIR_RECEIPT_BYTES)return EFS_ERR_PROTO;
    int rc=efs_repair_receipt_encode(j,kind,expected);if(rc)return rc;
    efs_hash(receipt,100,sum);if(memcmp(sum,receipt+100,32))return EFS_ERR_PROTO;
    return memcmp(expected,receipt,sizeof(expected))?EFS_ERR_STALE:EFS_OK;
}
int efs_repair_seal_authority(struct efs_kv *kv,const struct efs_repair_job *j)
{
    if(valid(j) || j->state!=EFS_REPAIR_ADMITTED || j->flags)return EFS_ERR_INVAL;
    struct efs_repair_job old;int rc=efs_repair_get(kv,j,&old);if(rc)return rc;
    return (old.flags&(EFS_REPAIR_DATA_DURABLE|EFS_REPAIR_CANCELLED))?EFS_OK:EFS_ERR_BUSY;
}
