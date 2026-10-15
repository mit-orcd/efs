/* Bounded GETATTR correctness/measurement on private KV fixtures only.
 * The same --measure source links against baseline and candidate libraries.
 * Default checks inject a lane change between reduction and validation. */
#include "efs/meta_apply.h"
#include "efs/metadata.h"
#include "efs/txn.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct observed {
    struct efs_kv *inner;
    uint64_t gets, scans;
    uint8_t lane[EFS_KV_KEY_MAX];
    uint32_t kl, lane_reads;
    int action, fired, fail_get, fail_scan;
    efs_ino_t ino;
};
static void be64(uint8_t *p, uint64_t x)
{ for (int i=7;i>=0;i--) {p[i]=(uint8_t)x;x>>=8;} }
static void put_row(struct efs_kv *kv, const struct efs_meta_row *row)
{
    uint8_t k[EFS_KV_KEY_MAX], v[EFS_META_INO_BYTES];uint32_t n=0;
    assert(!efs_kv_key_inode(efs_kv_inode_shard(row->ino),row->ino,k,&n));
    assert(!efs_meta_pack_inode(row,v,sizeof(v)));
    assert(!efs_kv_put(kv,k,n,v,sizeof(v)));
}
static int get(void *ctx,const uint8_t *k,uint32_t n,uint8_t *v,uint32_t *vn)
{
    struct observed *o=ctx;o->gets++;
    if(o->fail_get)return EFS_ERR_IO;
    if(n==o->kl && !memcmp(k,o->lane,n) && ++o->lane_reads==2 && o->action && !o->fired){
        o->fired=1;
        if(o->action==1){
            uint8_t image[64]={0};be64(image,257);be64(image+8,1001);
            be64(image+16,1001);be64(image+24,2);be64(image+56,1);
            assert(!efs_kv_put(o->inner,k,n,image,sizeof(image)));
        }else{
            struct efs_meta_row row;
            assert(!efs_meta_apply_get_inode(o->inner,o->ino,&row));
            row.generation++;row.active_lanes=0;row.base_size=7;
            row.content_epoch++;put_row(o->inner,&row);
        }
    }
    return efs_kv_get(o->inner,k,n,v,vn);
}
static int scan(void *ctx,const uint8_t *p,uint32_t n,
                int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *u)
{
    struct observed *o=ctx;o->scans++;
    return o->fail_scan?EFS_ERR_IO:efs_kv_scan_prefix(o->inner,p,n,cb,u);
}
static const struct efs_kv_ops ops={.get=get,.scan_prefix=scan};
static int coord(void *ctx,const struct efs_txid *t,uint32_t shard,int *out)
{ return efs_txn_decision_get(((struct observed *)ctx)->inner,shard,t,out); }
static void seed(struct observed *o,unsigned lanes)
{
    struct efs_meta_attrs at={.uid=1000,.gid=1000,.now=1000};
    struct efs_meta_row row;
    assert(!efs_meta_apply_init(o->inner,1000));
    assert(!efs_meta_apply_create_file(o->inner,&at,EFS_ROOT_INO,S_IFREG|0644,"f",&o->ino));
    assert(!efs_meta_apply_get_inode(o->inner,o->ino,&row));
    row.active_lanes=(1ull<<lanes)-1;put_row(o->inner,&row);
    for(unsigned i=0;i<lanes;i++){
        uint8_t k[EFS_KV_KEY_MAX],v[64]={0};uint32_t n=0;
        assert(!efs_kv_key_lane(efs_kv_lane_shard(o->ino,i),o->ino,row.generation,i,k,&n));
        be64(v,128);be64(v+8,1000);be64(v+16,1000);be64(v+24,1);be64(v+56,1);
        assert(!efs_kv_put(o->inner,k,n,v,sizeof(v)));
        if(!i){memcpy(o->lane,k,n);o->kl=n;}
    }
}
static void checks(void)
{
    for(int action=1;action<=2;action++){
        struct observed o={.inner=efs_kv_mem_create(),.action=action};assert(o.inner);
        seed(&o,1);struct efs_kv kv={.ops=&ops,.ctx=&o};struct efs_meta_stat st;
        assert(!efs_meta_apply_getattr(&kv,o.ino,coord,&o,&st));
        assert(o.fired && st.attempts==2 && st.size==(action==1?257u:7u));
        assert(st.alloc==(action==1?1u:0u));
        efs_kv_mem_free(o.inner);
    }
    struct observed o={.inner=efs_kv_mem_create()};assert(o.inner);seed(&o,1);
    struct efs_kv kv={.ops=&ops,.ctx=&o};struct efs_meta_stat st;
    o.fail_scan=1;assert(efs_meta_apply_getattr(&kv,o.ino,coord,&o,&st)==EFS_ERR_IO);
    o.fail_scan=0;o.fail_get=1;assert(efs_meta_apply_getattr(&kv,o.ino,coord,&o,&st)==EFS_ERR_IO);o.fail_get=0;
    uint8_t bad[23]={0};assert(!efs_kv_put(o.inner,o.lane,o.kl,bad,sizeof(bad)));
    assert(efs_meta_apply_getattr(&kv,o.ino,coord,&o,&st)==EFS_ERR_PROTO);
    assert(!efs_kv_del(o.inner,o.lane,o.kl));
    assert(!efs_meta_apply_getattr(&kv,o.ino,coord,&o,&st) && !st.size && !st.alloc);
    efs_kv_mem_free(o.inner);puts("GETATTR independent lane/row validation, absent/malformed lane and read/scan refusal PASS");
}
static uint64_t ns(void)
{ struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void measure(unsigned lanes, unsigned iterations)
{
    char dir[]="/tmp/efs-getattr-lane-XXXXXX";assert(mkdtemp(dir));
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_NOSYNC,.memtable_max=64u<<20,.l0_max=64};
    struct observed o={.inner=efs_kv_lsm_open(dir,&cfg)};assert(o.inner);seed(&o,lanes);
    /* Unrelated records give real immutable block/index searches. */
    for(unsigned i=0;i<4096;i++){
        uint8_t k[6]={0xff,0xff,0xfe,(uint8_t)(i>>8),(uint8_t)i,0},v[64]={0};
        assert(!efs_kv_put(o.inner,k,sizeof(k),v,sizeof(v)));
    }
    assert(!efs_kv_lsm_flush(o.inner));
    struct efs_kv kv={.ops=&ops,.ctx=&o};struct efs_meta_stat st;
    for(unsigned r=0;r<5;r++){
        o.gets=o.scans=0;uint64_t a=ns();
        for(unsigned i=0;i<iterations;i++){
            assert(!efs_meta_apply_getattr(&kv,o.ino,coord,&o,&st));
            assert(st.size==128 && st.alloc==lanes && st.attempts==1);
        }
        printf("{\"lanes\":%u,\"round\":%u,\"ns_per_stat\":%.1f,\"gets_per_stat\":%.1f,\"scans_per_stat\":%.1f}\n",
               lanes,r,(ns()-a)/(double)iterations,o.gets/(double)iterations,o.scans/(double)iterations);
    }
    efs_kv_lsm_close(o.inner);DIR *d=opendir(dir);assert(d);struct dirent *e;char p[1024];
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){
        snprintf(p,sizeof(p),"%s/%s",dir,e->d_name);assert(!unlink(p));
    }
    closedir(d);assert(!rmdir(dir));
}
int main(int argc,char **argv)
{
    if((argc==2 || argc==3) && !strcmp(argv[1],"--measure")){
        unsigned iterations=10000;
        if(argc==3){char *end;unsigned long n=strtoul(argv[2],&end,10);
            assert(*argv[2] && !*end && n>=1000 && n<=1000000);iterations=(unsigned)n;}
        measure(1,iterations);measure(8,iterations);return 0;
    }
    assert(argc==1);checks();return 0;
}
