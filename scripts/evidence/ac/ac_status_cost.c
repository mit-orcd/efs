/* AC measurement only: requested management snapshot cost at live counts.
 * Reuse the exact native test's transport handshake, not a mock registry. */
#define main ac_xprt_reference_main
#include "../../../tests/test_rdma_xprt.c"
#undef main
#include "../../../src/server/server_internal.h"
#include <time.h>
#include <assert.h>

static uint64_t ticks(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static int order(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
static void measure(unsigned live)
{
    uint64_t times[1000];struct efs_msg_io_stats_reply s;
    for(unsigned i=0;i<1000;i++){
        uint64_t t=ticks();efs_iostats_snapshot(&s);times[i]=ticks()-t;
        assert(s.rdma.live_conns==live);
    }
    qsort(times,1000,sizeof(times[0]),order);
    printf("status-cost live=%u p50_ns=%llu p99_ns=%llu max_ns=%llu\n",live,
           (unsigned long long)times[500],(unsigned long long)times[989],
           (unsigned long long)times[999]);
}
static void pair(struct efs_conn **cli,struct efs_conn **srv)
{
    int sv[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));
    *cli=efs_conn_wrap_tcp(sv[0],0);*srv=efs_conn_wrap_tcp(sv[1],1);assert(*cli&&*srv);
    struct timeval tv={.tv_sec=10};setsockopt(sv[1],SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct upgrade_req u={*cli,-1};pthread_t thread;
    assert(!pthread_create(&thread,NULL,upgrade_thread,&u));
    uint8_t type,*payload=NULL,reply[EFS_RMSG_SETUP_BYTES];uint32_t len=0,rlen=sizeof(reply);
    assert(!efs_recv_msg(sv[1],&type,(void **)&payload,&len));assert(type==EFS_MSG_RDMA_SETUP);
    assert(!efs_rdma_server_accept(*srv,payload,len,reply,&rlen));free(payload);
    assert(!efs_send_msg(sv[1],EFS_MSG_RDMA_SETUP_REPLY,reply,rlen));
    assert(!pthread_join(thread,NULL));assert(!u.rc);
}
int main(void)
{
    measure(0);
    if(!efs_rdma_available()){puts("NOTRUN: live connection status-cost requires RDMA");return 3;}
    struct efs_conn *connections[32];unsigned used=0;
    for(unsigned target=2;target<=32;target*=4){
        while(used<target){pair(&connections[used],&connections[used+1]);used+=2;}
        for(unsigned repeat=0;repeat<3;repeat++)measure(used);
    }
    for(unsigned i=0;i<used;i++)efs_conn_destroy(connections[i]);
    for(unsigned repeat=0;repeat<3;repeat++)measure(0);
    puts("PASS: requested snapshots retain zero/live/retired connection coverage");
    return 0;
}
