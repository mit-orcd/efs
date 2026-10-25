/* Compile the real dispatcher in this TU; no mocked apply implementation. */
#define main decoder_record_replay_main
#include "check.c"
#undef main
#include "../../src/server/raft_host.c"
#include <assert.h>
static int row_count(void *arg,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl)
{ (void)k;(void)kl;(void)v;(void)vl;(*(unsigned *)arg)++;return 0; }
int main(int argc,char **argv)
{
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,10):0;
    struct efs_raft_host *h=calloc(1,sizeof(*h));assert(h);
    h->kv=efs_kv_mem_create();assert(h->kv);
    uint64_t rounds=0;
    /* Every registered command must reject a one-byte command without
     * reading a missing body or touching KV. No live host state is needed. */
    assert(host_apply_dispatch(h,1,1,0,NULL,0)==EFS_ERR_UNSUPPORTED);
    for(unsigned tag=0;tag<256;tag++) {
        uint8_t *short_cmd=malloc(1);assert(short_cmd);*short_cmd=tag;
        int rc=host_apply_dispatch(h,1,1,0,short_cmd,1);
        assert(rc==EFS_ERR_UNSUPPORTED || rc==EFS_ERR_PROTO || rc==EFS_ERR_INVAL);
        free(short_cmd);
        unsigned rows=0;assert(!efs_kv_scan(h->kv,row_count,&rows));assert(!rows);
    }
    static const char *families[]={"degraded-command","recovery-command","pack-degraded",
        "ticket-batch","ticket-settle","ticket-drain"};
    struct timespec begin,now;clock_gettime(CLOCK_MONOTONIC,&begin);
    do {
        /* Random payloads for every unsupported tag, plus mutations of
         * the actual K32/K35/K36/K37/D31 encodings through dispatch. */
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
        uint8_t valid[8192],mutated[8192];
        size_t n=seed(families[next()%(sizeof(families)/sizeof(families[0]))],valid),len=n;
        memcpy(mutated,valid,n);
        switch(next()%4) {
        case 0:len=next()%(n+1);break;
        case 1:mutated[n]=0;len=n+1;break;
        default:for(unsigned i=0,k=1+next()%4;i<k;i++)mutated[next()%n]^=(uint8_t)(1+next()%255);break;
        }
        /* A changed tag is tested as unsupported, never as an unrelated
         * supported operation requiring a live runtime fixture. */
        if(len && mutated[0]!=valid[0])mutated[0]=200;
        uint8_t *p=malloc(len?len:1);assert(p);memcpy(p,mutated,len);
        int rc=host_apply_dispatch(h,1,1,0,p,len);free(p);
        assert(rc<=0);
        unsigned rows=0;assert(!efs_kv_scan(h->kv,row_count,&rows));assert(!rows);
        rounds++;
        clock_gettime(CLOCK_MONOTONIC,&now);
    } while(seconds && (now.tv_sec-begin.tv_sec)+(now.tv_nsec-begin.tv_nsec)/1e9<seconds);
    efs_kv_mem_free(h->kv);free(h);
    printf("PASS apply dispatch registered short commands, unknown tags and K32/K35/K36/K37/D31 mutations: %llu cases\n",(unsigned long long)rounds);
    return 0;
}
