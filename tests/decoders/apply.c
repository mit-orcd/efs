/* Compile the real dispatcher in this TU; no mocked apply implementation. */
#include "../../src/server/raft_host.c"
#include <assert.h>
static int row_count(void *arg,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl)
{ (void)k;(void)kl;(void)v;(void)vl;(*(unsigned *)arg)++;return 0; }
int main(int argc,char **argv)
{
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;
    struct efs_raft_host *h=calloc(1,sizeof(*h));assert(h);
    h->kv=efs_kv_mem_create();assert(h->kv);
    uint64_t rng=0x20261025c,rounds=0;
    struct timespec begin,now;clock_gettime(CLOCK_MONOTONIC,&begin);
    do {
        /* Unknown tags cover the real dispatch, zero-length covers its
         * entry guard. Known encodings below are exercised by the focused
         * record targets; do not fabricate an uninitialized live host. */
        for(unsigned tag=0;tag<256;tag++) {
            if(efs_md_cmd_applicable(tag))continue;
            rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
            uint32_t len=rng%512;uint8_t *p=malloc(len?len:1);assert(p);
            for(uint32_t i=0;i<len;i++)p[i]=(uint8_t)(rng>>((i%8)*8));
            p[0]=tag;
            assert(host_apply_dispatch(h,1,1,0,p,len)==EFS_ERR_UNSUPPORTED);
            free(p);
            unsigned rows=0;assert(!efs_kv_scan(h->kv,row_count,&rows));assert(!rows);rounds++;
        }
        clock_gettime(CLOCK_MONOTONIC,&now);
    } while(seconds && (now.tv_sec-begin.tv_sec)+(now.tv_nsec-begin.tv_nsec)/1e9<seconds);
    efs_kv_mem_free(h->kv);free(h);
    printf("PASS apply dispatch unknown-tag/empty-entry refusal: %llu cases\n",(unsigned long long)rounds);
    return 0;
}
