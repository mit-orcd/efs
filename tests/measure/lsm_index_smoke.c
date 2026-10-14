/* Small actual-LSM smoke/measurement, identical source against old/new libs.
 * No deployed stores. 128 disjoint L1 segments in one coarse range plus
 * 32 overlapping L0s. Timings cover warm point misses and reduction-prefix
 * misses; they are not filesystem throughput claims. */
#include "kv_lsm_internal.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void key(unsigned i, unsigned kind, uint8_t k[5])
{ k[0]=1;k[1]=(uint8_t)(i>>8);k[2]=(uint8_t)i;k[3]=(uint8_t)kind;k[4]=0; }
static uint64_t clock_ns(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static int count(void *p,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl)
{ (void)k;(void)kl;(void)v;(void)vl;(*(unsigned *)p)++;return 0; }
static struct efs_kv *open_kv(const char *dir)
{
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_NOSYNC,.memtable_max=1048576,.l0_max=64};
    struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);assert(kv);
    kv_compactor_stop(kv->ctx);return kv;
}
static void add_seg(struct kv_lsm *l,int level,unsigned i,int tombstone)
{
    char path[1200];uint64_t seq=l->next_seq++;
    kv_seg_path(l,level,seq,path,sizeof(path));struct kv_seg_w *w;struct kv_seg *s;
    assert(!kv_seg_w_open(path,&w));uint8_t k[5];key(i,0,k);
    if (i==UINT32_MAX) {
        uint8_t edge[2]={0,0};
        assert(!kv_seg_w_add(w,KV_OP_PUT,edge,2,(const uint8_t *)"edge",4));
        edge[0]=255;assert(!kv_seg_w_add(w,KV_OP_PUT,edge,2,(const uint8_t *)"edge",4));
    } else assert(!kv_seg_w_add(w,tombstone?KV_OP_DEL:KV_OP_PUT,k,5,(const uint8_t *)"value",5));
    assert(!kv_seg_w_finish(w));assert(!kv_seg_open(path,&s));
    struct seg_slot *slot;
    if(level){assert(!kv_l1_reserve(l,l->n_l1+1));slot=&l->l1[l->n_l1++];}
    else{assert(!kv_l0_reserve(l,l->n_l0+1));memmove(l->l0+1,l->l0,l->n_l0*sizeof(*l->l0));l->n_l0++;slot=l->l0;}
    memset(slot,0,sizeof(*slot));seg_slot_set(slot,s);slot->level=level;slot->seq=seq;
    /* Manifest publication is the external fixture's segment mutation point.
     * Production flush/compaction invalidate before their publication. */
    assert(!kv_manifest_write(l));
}
int main(void)
{
    char dir[]="/tmp/efs-lsm-index-smoke-XXXXXX";assert(mkdtemp(dir));
    struct efs_kv *kv=open_kv(dir);struct kv_lsm *l=kv->ctx;
    for(unsigned i=0;i<128;i++)add_seg(l,1,i,0);
    for(unsigned i=0;i<32;i++)add_seg(l,0,i,i==7);
    add_seg(l,0,UINT32_MAX,0); /* one legacy segment spanning coarse ranges */
    /* Reopen builds from the same persisted manifest in old and new engines. */
    efs_kv_lsm_close(kv);kv=open_kv(dir);l=kv->ctx;
    uint8_t k[5],value[32];uint32_t len;
    for(unsigned i=0;i<128;i++){
        key(i,0,k);len=sizeof(value);int rc=efs_kv_get(kv,k,5,value,&len);
        if(i==7)assert(rc==EFS_ERR_NOT_FOUND);else assert(!rc&&len==5&&!memcmp(value,"value",5));
    }
    unsigned total=0;assert(!efs_kv_scan_prefix(kv,NULL,0,count,&total));assert(total==129);
    for(unsigned i=0;i<128;i++){
        key(i,0,k);total=0;assert(!efs_kv_scan_prefix(kv,k,3,count,&total));assert(total==(i==7?0u:1u));
    }
    /* A retained private snapshot must not consult the live candidate map. */
    struct efs_kv_lsm_snapshot *snap=NULL;
    assert(!efs_kv_lsm_snapshot_pin(kv,16ull<<20,&snap));
    key(127,0,k);assert(!efs_kv_put(kv,k,5,(const uint8_t *)"new",3));
    assert(!efs_kv_lsm_flush(kv));
    total=0;assert(!efs_kv_lsm_snapshot_scan(snap,k,5,NULL,0,count,&total));assert(total==1);
    len=sizeof(value);assert(!efs_kv_lsm_snapshot_get(snap,k,5,value,&len)&&len==5&&!memcmp(value,"value",5));
    efs_kv_lsm_snapshot_release(snap);
    len=sizeof(value);assert(!efs_kv_get(kv,k,5,value,&len)&&len==3&&!memcmp(value,"new",3));
    /* Misses are representative of empty transaction-reduction prefixes. */
    for(unsigned round=0;round<5;round++){
        uint64_t a=clock_ns();
        for(unsigned i=0;i<10000;i++){key(i%128,1,k);len=sizeof(value);assert(efs_kv_get(kv,k,5,value,&len)==EFS_ERR_NOT_FOUND);}
        uint64_t b=clock_ns();
        for(unsigned i=0;i<10000;i++){key(i%128,1,k);total=0;assert(!efs_kv_scan_prefix(kv,k,4,count,&total)&&!total);}
        uint64_t c=clock_ns();
        printf("{\"round\":%u,\"l0\":%u,\"l1\":%u,\"point_ns\":%.1f,\"prefix_ns\":%.1f}\n",round,l->n_l0,l->n_l1,(b-a)/10000.,(c-b)/10000.);
    }
    /* Compaction changes candidate positions; cold reopen preserves results. */
    assert(!efs_kv_lsm_compact(kv));efs_kv_lsm_close(kv);kv=open_kv(dir);
    key(7,0,k);len=sizeof(value);assert(efs_kv_get(kv,k,5,value,&len)==EFS_ERR_NOT_FOUND);
    key(127,0,k);len=sizeof(value);assert(!efs_kv_get(kv,k,5,value,&len)&&len==3);
    /* Update the boundary keys, then supersede/tombstone an existing row
     * after the live index was populated. */
    for(unsigned i=0;i<2;i++){
        uint8_t boundary[2]={(uint8_t)(i?255:0),0};
        assert(!efs_kv_put(kv,boundary,2,(const uint8_t *)"edge",4));
    }
    assert(!efs_kv_lsm_flush(kv));
    for(unsigned i=0;i<2;i++){
        uint8_t boundary[2]={(uint8_t)(i?255:0),0};len=sizeof(value);
        assert(!efs_kv_get(kv,boundary,2,value,&len)&&len==4&&!memcmp(value,"edge",4));
        total=0;assert(!efs_kv_scan_prefix(kv,boundary,1,count,&total)&&total==1);
    }
    key(7,0,k);assert(!efs_kv_put(kv,k,5,(const uint8_t *)"revived",7));assert(!efs_kv_lsm_flush(kv));
    len=sizeof(value);assert(!efs_kv_get(kv,k,5,value,&len)&&len==7);
    assert(!efs_kv_del(kv,k,5));assert(!efs_kv_lsm_flush(kv));
    len=sizeof(value);assert(efs_kv_get(kv,k,5,value,&len)==EFS_ERR_NOT_FOUND);
    efs_kv_lsm_close(kv);
    DIR *d=opendir(dir);assert(d);struct dirent *e;char path[1200];
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
    closedir(d);assert(!rmdir(dir));puts("LSM candidate smoke PASS");return 0;
}
