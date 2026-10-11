/* W96: actual production WAL, same durable records with/without reservation.
 * Compile EFS_KV_WAL_RESERVE_BYTES=0 for control. No production mode switch. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "kv_lsm_internal.h"
static uint64_t histogram[1000001], barriers, sync_ns, sync_max;
static uint64_t now(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static int measured_sync(int fd) {
    uint64_t t=now(); int rc=fdatasync(fd); t=now()-t;
    histogram[t/1000<1000000?t/1000:1000000]++;
    barriers++; sync_ns+=t; if(t>sync_max)sync_max=t;
    return rc;
}
#define fdatasync measured_sync
#include "kv_wal.c"
#undef fdatasync
static uint64_t replayed;
static int verify(void *u,uint8_t op,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl) {
    (void)u; assert(op==KV_OP_PUT && kl==8 && vl==128);
    uint64_t id; memcpy(&id,k,8);
    for(unsigned j=0;j<vl;j++) assert(v[j]==(uint8_t)(id+j));
    replayed++; return EFS_OK;
}
static uint64_t percentile(unsigned p) {
    uint64_t target=(barriers*p+99)/100, n=0;
    for(unsigned i=0;i<=1000000;i++){n+=histogram[i];if(n>=target)return i;}
    abort();
}
int main(int argc,char **argv) {
    assert(argc==4); unsigned batch=(unsigned)strtoul(argv[3],NULL,10);
    assert(batch==1 || batch==16); double seconds=strtod(argv[2],NULL);assert(seconds>0);
    int fd=open(argv[1],O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);assert(!close(fd));
    struct kv_wal *w;assert(kv_wal_open(argv[1],EFS_KV_LSM_SYNC,&w)==EFS_OK);
    struct efs_kv_item items[16]; uint64_t keys[16];uint8_t vals[16][128];
    uint64_t seq=0, records=0, pending=0, resets=0, max_slack=0,max_alloc=0,bytes=0;
    uint64_t origin=now(), deadline=origin+(uint64_t)(seconds*1e9);
    uint64_t last=origin, last_records=0, last_calls=0, last_ns=0;
    while(now()<deadline) {
        for(unsigned i=0;i<batch;i++) {
            keys[i]=records*16+i;
            for(unsigned j=0;j<128;j++)vals[i][j]=(uint8_t)(keys[i]+j);
            items[i]=(struct efs_kv_item){EFS_KV_PUT,(uint8_t*)&keys[i],8,vals[i],128};
        }
        uint64_t old=w->bytes; assert(kv_wal_append(w,items,batch,&seq)==EFS_OK);
        assert(w->synced>=seq); records++;pending+=batch;bytes+=w->bytes-old;
        uint64_t slack=w->allocated_bytes-w->bytes;
        if(slack>max_slack)max_slack=slack;
        struct stat s;assert(!fstat(w->fd,&s));
        if((uint64_t)s.st_blocks*512>max_alloc)max_alloc=(uint64_t)s.st_blocks*512;
        if(w->bytes>=8*1024*1024) {
            replayed=0;assert(kv_wal_replay(argv[1],verify,NULL)==EFS_OK && replayed==pending);
            assert(kv_wal_reset(w,seq)==EFS_OK && w->bytes==0 && w->allocated_bytes==0);
            resets++;pending=0;
        }
        uint64_t t=now();
        if(t-last>=30000000000ull) {
            fprintf(stderr,"{\"seconds\":%.3f,\"records_s\":%.6f,\"sync_mean_ms\":%.6f,\"sync_calls\":%llu}\n",(t-origin)/1e9,(records-last_records)/((t-last)/1e9),(sync_ns-last_ns)/1e6/(barriers-last_calls),(unsigned long long)(barriers-last_calls));
            fflush(stderr);last=t;last_records=records;last_calls=barriers;last_ns=sync_ns;
        }
    }
    uint64_t elapsed=now()-origin;kv_wal_close(w);
    replayed=0;assert(kv_wal_replay(argv[1],verify,NULL)==EFS_OK && replayed==pending);
    /* Reopen behind repaired reserved EOF, append one new durable record and
     * replay again: valid bytes must remain reachable across restart. */
    assert(kv_wal_open(argv[1],EFS_KV_LSM_SYNC,&w)==EFS_OK);
    assert(kv_wal_append(w,items,batch,&seq)==EFS_OK);kv_wal_close(w);
    replayed=0;assert(kv_wal_replay(argv[1],verify,NULL)==EFS_OK && replayed==pending+batch);
    printf("{\"reserve_bytes\":%llu,\"batch\":%u,\"records\":%llu,\"seconds\":%.6f,\"records_s\":%.6f,\"encoded_bytes\":%llu,\"resets\":%llu,\"max_reserved_slack\":%llu,\"max_allocated_bytes\":%llu,\"sync_calls_including_reopen\":%llu,\"sync_mean_ms\":%.6f,\"sync_p50_us\":%llu,\"sync_p95_us\":%llu,\"sync_p99_us\":%llu,\"sync_max_ms\":%.6f,\"replay_verified\":true}\n",
      (unsigned long long)EFS_KV_WAL_RESERVE_BYTES,batch,(unsigned long long)records,elapsed/1e9,records/(elapsed/1e9),(unsigned long long)bytes,(unsigned long long)resets,(unsigned long long)max_slack,(unsigned long long)max_alloc,(unsigned long long)barriers,sync_ns/1e6/barriers,(unsigned long long)percentile(50),(unsigned long long)percentile(95),(unsigned long long)percentile(99),sync_max/1e6);
    return 0;
}
