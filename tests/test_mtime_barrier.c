/* Exact-image mtime fencing, publication races, and durable decision recovery. */
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int decision(void *ctx,const struct efs_txid *t,uint32_t sh,int *d)
{ return efs_txn_decision_get(ctx,sh,t,d); }
static int prepare(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,
                   const struct efs_meta_mtime_image *q)
{
    return efs_txn_prepare_excl_value(ctx,t,p,q->key,q->klen,q->expected,q->expected_len,
                                     EFS_TXN_PUT,q->value,q->new_len);
}
static int decide(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{ return efs_txn_decide(ctx,efs_txn_coordinator(t,p),t,d); }
static int resolve(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{
    for(unsigned i=0;i<p->n;++i) {
        int rc=efs_txn_resolve(ctx,t,p->shard[i],d); if(rc!=EFS_OK)return rc;
    }
    return EFS_OK;
}
static const struct efs_meta_mtime_ops ops={prepare,decide,resolve};
static int lost_decision(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{
    assert(decide(ctx,t,p,d)==EFS_OK);
    return EFS_ERR_IO; /* durable decision, lost acknowledgement */
}

static void bootstrap(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t lane,unsigned id)
{
    struct efs_meta_lane_bootstrap q; struct efs_txid t={{0}};t.bytes[0]=id;
    assert(efs_meta_capture_lane_bootstrap(kv,ino,gen,lane,EFS_MIN_CHUNK_SIZE,decision,kv,&q)==EFS_OK);
    struct efs_txn_parts p={0}; p.shard[p.n++]=efs_kv_inode_shard(ino);
    uint32_t sh=efs_kv_lane_shard(ino,lane);if(sh!=p.shard[0])p.shard[p.n++]=sh;
    for(unsigned i=0;i<2;++i) {
        uint8_t key[EFS_KV_KEY_MAX],pay[EFS_META_LANE_BOOTSTRAP_BYTES];uint32_t kl;
        assert(efs_meta_encode_lane_bootstrap(&q,i?lane:EFS_META_FENCE_INODE,key,&kl,pay)==EFS_OK);
        assert(efs_txn_apply_prepare(kv,EFS_TXN_LANE_BOOTSTRAP,&t,&p,key,kl,pay,sizeof(pay))==EFS_OK);
    }
    assert(decide(kv,&t,&p,EFS_TXN_COMMIT)==EFS_OK);
    assert(resolve(kv,&t,&p,EFS_TXN_COMMIT)==EFS_OK);
}
static struct efs_meta_pub publication(efs_ino_t ino,uint64_t gen,unsigned ci)
{
    struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.chunk_index=ci;
    p.lane_local=1;p.new_size=1;p.now=500;p.candidate_gen=70;
    p.coding_profile_id=EFS_META_PROFILE_K2F1;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;++i)p.ch.nodes[i]=i+1;
    return p;
}
static void fixture(struct efs_kv *kv,efs_ino_t *ino,uint64_t *gen)
{
    struct efs_meta_attrs a={1000,1000,100};struct efs_meta_row row;
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&a,EFS_ROOT_INO,S_IFREG|0644,"file",ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,*ino,&row)==EFS_OK);*gen=row.generation;
    bootstrap(kv,*ino,*gen,17,1);
    struct efs_meta_pub p=publication(*ino,*gen,17);
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
}
static void unit(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino;uint64_t gen;
    fixture(kv,&ino,&gen);
    struct efs_meta_mtime_plan *plan=malloc(sizeof(*plan));assert(plan);
    assert(efs_meta_capture_mtime(kv,ino,600,EFS_META_SET_MTIME|EFS_META_SET_ATIME,10,20,decision,kv,plan)==EFS_OK);
    struct efs_txid t={{2}};struct efs_meta_writer_view view;
    assert(prepare(kv,&t,&plan->parts,&plan->images[0])==EFS_OK);
    /* The inode hold closes the active-lane set against cold bootstrap. */
    struct efs_meta_lane_bootstrap q;
    assert(efs_meta_capture_lane_bootstrap(kv,ino,gen,18,EFS_MIN_CHUNK_SIZE,decision,kv,&q)==EFS_OK);
    uint8_t bk[EFS_KV_KEY_MAX],bp[EFS_META_LANE_BOOTSTRAP_BYTES]; uint32_t bkl;
    assert(efs_meta_encode_lane_bootstrap(&q,EFS_META_FENCE_INODE,bk,&bkl,bp)==EFS_OK);
    struct efs_txid competing={{99}};
    struct efs_txn_parts bparts={0};bparts.shard[bparts.n++]=efs_kv_inode_shard(ino);
    uint32_t bsh=efs_kv_lane_shard(ino,18);if(bsh!=bparts.shard[0])bparts.shard[bparts.n++]=bsh;
    assert(efs_txn_apply_prepare(kv,EFS_TXN_LANE_BOOTSTRAP,&competing,&bparts,bk,bkl,bp,sizeof(bp))==EFS_ERR_BUSY);
    assert(prepare(kv,&t,&plan->parts,&plan->images[1])==EFS_OK);
    assert(efs_meta_get_lane_writer_view(kv,ino,gen,17,EFS_MIN_CHUNK_SIZE,&view)==EFS_ERR_BUSY);
    struct efs_meta_pub p=publication(ino,gen,81);p.now=700;
    assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_BUSY);
    assert(decide(kv,&t,&plan->parts,EFS_TXN_COMMIT)==EFS_OK);
    /* A committed but unresolved lane remains unavailable to publication. */
    assert(efs_meta_get_lane_writer_view(kv,ino,gen,17,EFS_MIN_CHUNK_SIZE,&view)==EFS_ERR_BUSY);
    assert(resolve(kv,&t,&plan->parts,EFS_TXN_COMMIT)==EFS_OK);
    struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK);
    assert(st.mtime==10 && st.atime==20 && st.ctime==600 && st.size==1);
    /* A stale legacy REPORT cannot resurrect pre-utimens mtime. */
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.mtime==10 && st.ctime==700);
    p=publication(ino,gen,145);p.mtime_gen=1;p.now=800;
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.mtime==800);
    /* Cold bootstrap copies the current mtime generation. */
    bootstrap(kv,ino,gen,18,3);
    uint8_t key[EFS_KV_KEY_MAX],val[EFS_META_LANE_BYTES];uint32_t kl,vl=sizeof(val);
    assert(efs_kv_key_lane(efs_kv_lane_shard(ino,18),ino,gen,18,key,&kl)==EFS_OK);
    assert(efs_kv_get(kv,key,kl,val,&vl)==EFS_OK && val[47]==1);
    /* Publication racing capture loses exact lane CAS, aborts inode changes. */
    assert(efs_meta_capture_mtime(kv,ino,900,EFS_META_SET_MTIME,5,0,decision,kv,plan)==EFS_OK);
    p=publication(ino,gen,209);p.mtime_gen=1;p.now=850;
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);t.bytes[0]=4;
    assert(efs_meta_execute_mtime(plan,&t,&ops,kv)==EFS_ERR_STALE);
    assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.mtime==850);
    /* A fresh full barrier preserves data and deliberately sets time backwards. */
    assert(efs_meta_capture_mtime(kv,ino,900,EFS_META_SET_MTIME,5,0,decision,kv,plan)==EFS_OK);
    t.bytes[0]=5;assert(efs_meta_execute_mtime(plan,&t,&ops,kv)==EFS_OK);
    assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.mtime==5 && st.atime==20);
    assert(efs_meta_capture_mtime(kv,ino,950,EFS_META_SET_MTIME,4,0,decision,kv,plan)==EFS_OK);
    /* Reject an incomplete participant set before preparing anything. */
    unsigned count=plan->count;plan->count=1;t.bytes[0]=6;
    assert(efs_meta_execute_mtime(plan,&t,&ops,kv)==EFS_ERR_INVAL);plan->count=count;
    struct efs_meta_mtime_ops lost_ops={prepare,lost_decision,resolve};
    assert(efs_meta_execute_mtime(plan,&t,&lost_ops,kv)==EFS_ERR_IO);
    int d=0;assert(efs_txn_decision_get(kv,efs_txn_coordinator(&t,&plan->parts),&t,&d)==EFS_OK && d==EFS_TXN_COMMIT);
    assert(efs_meta_get_lane_writer_view(kv,ino,gen,17,EFS_MIN_CHUNK_SIZE,&view)==EFS_ERR_BUSY);
    assert(resolve(kv,&t,&plan->parts,EFS_TXN_COMMIT)==EFS_OK);
    assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.mtime==4);
    free(plan);efs_kv_mem_free(kv);
}
static void recovery(int verdict)
{
    char dir[]="/tmp/efs-mtime-XXXXXX";assert(mkdtemp(dir));
    struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);efs_ino_t ino;uint64_t gen;
    fixture(kv,&ino,&gen);efs_kv_lsm_close(kv);
    struct efs_txid t={{9}};pid_t child=fork();assert(child>=0);
    if(!child) {
        kv=efs_kv_lsm_open(dir,NULL);assert(kv);
        struct efs_meta_mtime_plan *plan=malloc(sizeof(*plan));assert(plan);
        assert(efs_meta_capture_mtime(kv,ino,600,EFS_META_SET_MTIME,10,0,decision,kv,plan)==EFS_OK);
        for(unsigned i=0;i<plan->count;++i)assert(prepare(kv,&t,&plan->parts,&plan->images[i])==EFS_OK);
        if(verdict)assert(decide(kv,&t,&plan->parts,verdict)==EFS_OK);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);struct efs_meta_writer_view view;
    assert(efs_meta_get_lane_writer_view(kv,ino,gen,17,EFS_MIN_CHUNK_SIZE,&view)==EFS_ERR_BUSY);
    struct efs_txn_parts parts={0};parts.shard[parts.n++]=efs_kv_inode_shard(ino);
    uint32_t lane=efs_kv_lane_shard(ino,17);if(lane!=parts.shard[0])parts.shard[parts.n++]=lane;
    if(!verdict){verdict=EFS_TXN_ABORT;assert(decide(kv,&t,&parts,verdict)==EFS_OK);}
    /* Resolve lane first, crash/reopen before inode resolve. */
    assert(efs_txn_resolve(kv,&t,lane,verdict)==EFS_OK);efs_kv_lsm_close(kv);
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    assert(resolve(kv,&t,&parts,verdict)==EFS_OK);
    struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK);
    assert(st.mtime==(verdict==EFS_TXN_COMMIT?10:500));
    assert(efs_meta_get_lane_writer_view(kv,ino,gen,17,EFS_MIN_CHUNK_SIZE,&view)==EFS_OK);
    efs_kv_lsm_close(kv);DIR *d=opendir(dir);assert(d);struct dirent *e;
    while((e=readdir(d))){if(e->d_name[0]=='.')continue;char path[512];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
    closedir(d);assert(!rmdir(dir));
}
int main(void){unit();recovery(EFS_TXN_COMMIT);recovery(EFS_TXN_ABORT);recovery(0);puts("mtime barrier publication races and durable recovery PASS");}
