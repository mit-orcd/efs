/* Production-cap W23 engine experiment. Run only in an owned empty directory.
 * Links a fault-build libefs.a; does not lower the 1 GiB L0 cap. */
#include <efs/common.h>
#include <efs/kv.h>
#include <efs/kv_lsm.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static unsigned long rss(void) {
    FILE *f=fopen("/proc/self/status","r");char line[256];unsigned long n=0;
    assert(f);while(fgets(line,sizeof(line),f))if(sscanf(line,"VmRSS: %lu",&n)==1)break;
    fclose(f);return n;
}
static void sample(struct efs_kv *kv,const char *phase,unsigned n) {
    struct efs_kv_lsm_stats s;assert(efs_kv_lsm_stats(kv,&s)==EFS_OK);
    printf("sample phase=%s writes=%u rss_kib=%lu mt_bytes=%llu l0_bytes=%llu l0=%u l1=%u hot=%d\n",
      phase,n,rss(),(unsigned long long)s.mt_bytes,(unsigned long long)s.l0_bytes,s.n_l0,s.n_l1,efs_kv_lsm_l0_hot(kv));fflush(stdout);
}
int main(int argc,char **argv) {
    assert(argc==3);setenv("EFS_FAULT_COMPACT_STALL","1",1);setenv("EFS_FAULT_COMPACT_FILE",argv[2],1);
    FILE *fault=fopen(argv[2],"wx");assert(fault);fclose(fault);
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);usleep(300000);
    unsigned size=8u<<20,n=0;uint8_t *body=malloc(size),*got=malloc(size);assert(body&&got);memset(body,0x5a,size);
    sample(kv,"baseline",n);
    for(;n<150&&!efs_kv_lsm_l0_hot(kv);n++) {
        uint8_t key[8]={1,0,(uint8_t)(n>>8),(uint8_t)n};
        assert(efs_kv_put(kv,key,sizeof(key),body,size)==EFS_OK);
        if(n%16==15)sample(kv,"fill",n+1);
    }
    assert(efs_kv_lsm_l0_hot(kv));sample(kv,"cap",n);
    /* A finite 64 MiB post-cap tail measures growth; not a claim of a bound.
     * Keep these keys disjoint from the initial segment set. */
    for(unsigned i=0;i<8;i++) {
        uint8_t key[8]={2,0,0,(uint8_t)i};
        assert(efs_kv_put(kv,key,sizeof(key),body,size)==EFS_OK);sample(kv,"deferred",i+1);
    }
    struct efs_kv_lsm_stats s;assert(!efs_kv_lsm_stats(kv,&s));assert(s.mt_bytes>=64u<<20);
    assert(!unlink(argv[2]));time_t start=time(NULL);
    /* No further writes or maintenance flushes: compactor must drain alone. */
    do {usleep(100000);assert(!efs_kv_lsm_stats(kv,&s));assert(time(NULL)-start<120);}
    while(s.mt_bytes>=4u<<20 || s.n_l0>=4);
    assert(!efs_kv_lsm_quiesce(kv));sample(kv,"recovered",8);
    printf("recovery_seconds=%ld\n",(long)(time(NULL)-start));
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    for(unsigned i=0;i<8;i++) {uint8_t key[8]={2,0,0,(uint8_t)i};uint32_t len=size;
        assert(!efs_kv_get(kv,key,sizeof(key),got,&len)&&len==size&&!memcmp(got,body,size));}
    efs_kv_lsm_close(kv);free(got);free(body);
    puts("production 1-GiB cap, finite post-cap growth, idle drain and durable reopen PASS");return 0;
}
