/* Task AI: real-verbs, same-process request/reply streams plus concurrent
 * connection destruction/reuse. Not a filesystem or multi-host throughput gate.
 * Compile against the native and disposable diagnostic libefs.a separately.
 * Usage: binary STREAMS ROUNDS CHURN_CYCLES (1..32, >0, >=0).
 * Every 4096-byte echo is compared. TCP is a separate control, not an RDMA pass.
 */
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"AI streams FAIL line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static pthread_barrier_t ready;
static int use_rdma, rounds;
static int active_load;
struct stream { int fd, server, id, churn; };
static uint64_t now_ns(void) {
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static struct efs_conn *setup(struct stream *s) {
    struct efs_conn *c=efs_conn_wrap_tcp(s->fd,s->server); CHECK(c);
    if (use_rdma) {
        if (!s->server) CHECK(!efs_rdma_client_upgrade(c));
        else {
            uint8_t type,reply[EFS_RMSG_SETUP_BYTES]; void *p=NULL;
            uint32_t n=0,len=sizeof(reply);
            CHECK(!efs_recv_msg(s->fd,&type,&p,&n) && type==EFS_MSG_RDMA_SETUP);
            CHECK(!efs_rdma_server_accept(c,p,n,reply,&len)); free(p);
            CHECK(!efs_send_msg(s->fd,EFS_MSG_RDMA_SETUP_REPLY,reply,len));
        }
        CHECK(c->rc);
    }
    c->recv_timeout_ms=10000;
    return c;
}
static void exchange(struct efs_conn *c,int server,int id,int count) {
    unsigned char data[4096];
    for (int r=0;r<count;r++) {
        uint8_t type; void *p=NULL; uint32_t n=0;
        if (server) {
            CHECK(!efs_conn_recv_msg(c,&type,&p,&n));
            CHECK(type==42 && n==sizeof(data));
            CHECK(!efs_conn_send_msg(c,43,p,n)); free(p);
        } else {
            memset(data,(unsigned char)(id+r),sizeof(data));
            memcpy(data,&r,sizeof(r)); memcpy(data+sizeof(r),&id,sizeof(id));
            CHECK(!efs_conn_send_msg(c,42,data,sizeof(data)));
            CHECK(!efs_conn_recv_msg(c,&type,&p,&n));
            CHECK(type==43 && n==sizeof(data) && !memcmp(p,data,n)); free(p);
        }
    }
}
static void *load(void *arg) {
    struct stream *s=arg; struct efs_conn *c=setup(s);
    int br=pthread_barrier_wait(&ready); CHECK(!br || br==PTHREAD_BARRIER_SERIAL_THREAD);
    __atomic_add_fetch(&active_load,1,__ATOMIC_SEQ_CST);
    br=pthread_barrier_wait(&ready); CHECK(!br || br==PTHREAD_BARRIER_SERIAL_THREAD);
    exchange(c,s->server,s->id,rounds);
    efs_conn_destroy(c);
    __atomic_sub_fetch(&active_load,1,__ATOMIC_SEQ_CST); return NULL;
}
static void *churn(void *arg) {
    struct stream *s=arg;
    struct efs_conn *c=setup(s); exchange(c,s->server,s->id,4);
    efs_conn_destroy(c); return NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==4);
    int streams=atoi(argv[1]), cycles=atoi(argv[3]); rounds=atoi(argv[2]);
    CHECK(streams>=1 && streams<=32 && rounds>0 && cycles>=0);
    use_rdma=getenv("EFS_TRANSPORT") && !strcmp(getenv("EFS_TRANSPORT"),"rdma");
    if (use_rdma && !efs_rdma_available()) {
        fprintf(stderr,"NOTRUN: usable real RDMA device required\n"); return 77;
    }
    CHECK(!pthread_barrier_init(&ready,NULL,(unsigned)(2*streams+1)));
    pthread_t ts[64]; struct stream s[64];
    for(int i=0;i<streams;i++) {
        int fd[2]; CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
        for(int j=0;j<2;j++) {
            int k=2*i+j; s[k]=(struct stream){.fd=fd[j],.server=j,.id=i};
            CHECK(!pthread_create(&ts[k],NULL,load,&s[k]));
        }
    }
    int br=pthread_barrier_wait(&ready); CHECK(!br || br==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t start=now_ns();
    br=pthread_barrier_wait(&ready); CHECK(!br || br==PTHREAD_BARRIER_SERIAL_THREAD);
    int overlapping_cycles=0;
    for(int i=0;i<cycles;i++) {
        if (__atomic_load_n(&active_load,__ATOMIC_SEQ_CST)>0) overlapping_cycles++;
        int fd[2]; CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
        pthread_t ct[2]; struct stream cs[2];
        for(int j=0;j<2;j++) {
            cs[j]=(struct stream){.fd=fd[j],.server=j,.id=100+i};
            CHECK(!pthread_create(&ct[j],NULL,churn,&cs[j]));
        }
        for(int j=0;j<2;j++) CHECK(!pthread_join(ct[j],NULL));
    }
    for(int i=0;i<2*streams;i++) CHECK(!pthread_join(ts[i],NULL));
    uint64_t elapsed=now_ns()-start;
    struct efs_rdma_totals totals={0}; efs_rdma_totals(&totals);
    CHECK(!efs_rdma_live_conns() && !efs_rdma_assembly_bytes());
    CHECK(!use_rdma || (totals.data_frames_tx && totals.data_frames_rx));
    CHECK(!cycles || overlapping_cycles>0);
    printf("AI_STREAMS {\"verdict\":\"PASS_SCOPED\",\"rdma\":%d,\"streams\":%d,\"rounds\":%d,"
           "\"churn_cycles\":%d,\"overlapping_cycles\":%d,\"elapsed_ns\":%llu,\"echoes\":%d,\"rdma_frames_tx\":%llu,\"rdma_frames_rx\":%llu}\n",
           use_rdma,streams,rounds,cycles,overlapping_cycles,(unsigned long long)elapsed,streams*rounds+cycles*4,
           (unsigned long long)totals.data_frames_tx,(unsigned long long)totals.data_frames_rx);
    return 0;
}
