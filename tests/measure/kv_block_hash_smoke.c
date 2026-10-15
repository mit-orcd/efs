/* Actual LSM point-read A/B: link unchanged source against old/new libraries.
 * 32 overlapping L0 segments, 1024 long metadata-like keys each, 16-byte values.
 * Private NOSYNC fixture, compactor paused; no production stores touched. */
#include "../../src/kv/kv_lsm_internal.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static void key(unsigned id,uint8_t k[20])
{ memset(k,0,20);k[0]=1;k[2]=7;for(int j=19;j>=12;--j){k[j]=(uint8_t)id;id>>=8;} }
static uint64_t ns(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(void)
{
    char dir[]="/tmp/efs-block-hash-lsm-XXXXXX";assert(mkdtemp(dir));
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_NOSYNC,.memtable_max=64u<<20,.l0_max=64};
    struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);assert(kv);kv_compactor_stop(kv->ctx);
    uint8_t k[20],v[16],got[16];
    for(unsigned segment=0;segment<32;++segment){
        memset(v,segment,16);
        for(unsigned i=0;i<1024;++i){key(i*64+segment*2,k);assert(!efs_kv_put(kv,k,20,v,16));}
        assert(!efs_kv_lsm_flush(kv));
    }
    assert(((struct kv_lsm *)kv->ctx)->n_l0==32);
    for(unsigned round=0;round<6;++round){
        uint64_t a=ns();unsigned hits=0;
        for(unsigned i=0;i<50000;++i){unsigned id=(i*313u)&65535;key(id,k);uint32_t len=16;
            int rc=efs_kv_get(kv,k,20,got,&len);
            if(id&1)assert(rc==EFS_ERR_NOT_FOUND);
            else{assert(!rc && len==16);for(unsigned j=0;j<16;++j)assert(got[j]==(id%64)/2);hits++;}
        }
        printf("{\"round\":%u,\"point_ns\":%.2f,\"hits\":%u,\"operations\":50000}\n",round,(ns()-a)/50000.,hits);
    }
    efs_kv_lsm_close(kv);DIR *d=opendir(dir);assert(d);struct dirent *e;char p[1024];
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){
        snprintf(p,sizeof(p),"%s/%s",dir,e->d_name);assert(!unlink(p));}
    closedir(d);assert(!rmdir(dir));
}
