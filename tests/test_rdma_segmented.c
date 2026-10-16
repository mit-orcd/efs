/* Two processes, unequal physical receive pools; no pre-fork verbs state.
 * Requires real verbs device (exit77 is NOTRUN), uses socketpair for setup. */
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include "efs/network.h"
#include "efs/protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#define OK(x) do { if (!(x)) {fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);} } while(0)
static void pattern(unsigned char *p,unsigned n,unsigned seed)
{ for(unsigned i=0;i<n;i++) p[i]=(unsigned char)(i*131u+seed); }
static void verify(const unsigned char *p,unsigned n,unsigned seed)
{ for(unsigned i=0;i<n;i++) OK(p[i]==(unsigned char)(i*131u+seed)); }
static void check_tcp_empty(int fd)
{
    unsigned char b;errno=0;
    ssize_t n=recv(fd,&b,1,MSG_PEEK|MSG_DONTWAIT);
    OK(n==0 || (n==-1 && (errno==EAGAIN || errno==EWOULDBLOCK))); /* FIN is not application payload */
}
static struct efs_conn *setup(int fd,int server)
{
    struct efs_conn *c=efs_conn_wrap_tcp(fd,server);OK(c);
    if (!server) OK(!efs_rdma_client_upgrade(c));
    else {
        uint8_t t,rep[EFS_RMSG_SETUP_BYTES];void *p=NULL;uint32_t n=0,r=sizeof(rep);
        OK(!efs_recv_msg(fd,&t,&p,&n) && t==EFS_MSG_RDMA_SETUP);
        OK(!efs_rdma_server_accept(c,p,n,rep,&r));free(p);
        OK(!efs_send_msg(fd,EFS_MSG_RDMA_SETUP_REPLY,rep,r));
    }
    OK(c->rc);c->recv_timeout_ms=5000;return c;
}
static void duplex(struct efs_conn *c,int server)
{
    unsigned n=8*1024*1024;unsigned char *p=malloc(n);OK(p);pattern(p,n,server?17:29);
    /* Both endpoints send first, without a consumer/application reply. */
    OK(!efs_conn_send_msg_parts(c,42,p,3,p+3,n-3));memset(p,0,n);free(p);
    void *reply=NULL;uint8_t t;uint32_t got;
    OK(!efs_conn_recv_msg(c,&t,&reply,&got));OK(t==42 && got==n);
    verify(reply,got,server?29:17);free(reply);check_tcp_empty(c->fd);
}
static int reconnect_churn(void)
{
    enum { N=16 };int sockets[N][2];
    for(int i=0;i<N;i++) OK(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets[i]));
    pid_t pid=fork();OK(pid>=0);int server=pid==0;
    setenv("EFS_RDMA_BUFS",server?"2":"7",1);
    if (!efs_rdma_available()) {
        if (!server) {int st;waitpid(pid,&st,0);puts("SKIP: no RDMA device; churn NOTRUN");}
        return 0;
    }
    for(int i=0;i<N;i++) close(sockets[i][server?0:1]);
    for(int i=0;i<N;i++) {
        struct efs_conn *c=setup(sockets[i][server?1:0],server);
        if (!(i&1)) duplex(c,server);
        else if (server) {
            unsigned n=16*1024*1024;unsigned char *p=calloc(1,n);OK(p);
            OK(efs_conn_send_msg(c,42,p,n)!=0);free(p);
        } else {
            for(unsigned j=0;j<10000 && !efs_rdma_assembly_bytes();j++) {
                OK(efs_rdma_message_ready(c->rc)==0);usleep(100);
            }
            OK(efs_rdma_assembly_bytes()>0);
        }
        efs_conn_destroy(c);OK(!efs_rdma_live_conns() && !efs_rdma_assembly_bytes());
    }
    if (!server) {
        int st;OK(waitpid(pid,&st,0)==pid && WIFEXITED(st) && WEXITSTATUS(st)==0);
        puts("PASS: same-process16 reconnects,8 duplex/8 interrupted, reused registry generations, zero live connections/assemblies each cycle");
    }
    return 0;
}
int main(int argc,char **argv)
{
    const char *mode=argc>1?argv[1]:"normal";
    if (!strcmp(mode,"churn")) return reconnect_churn();
    int sv[2];OK(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));
    pid_t pid=fork();OK(pid>=0);int server=pid==0;
    /* burst-asym: the sender has the small pool, so the peer's CREDIT
     * frames for our DATA land in two receives. */
    if (!strcmp(mode,"burst-asym")) setenv("EFS_RDMA_BUFS",server?"7":"2",1);
    else if (strcmp(mode,"burst")) setenv("EFS_RDMA_BUFS",server?"2":"7",1);
    if (!efs_rdma_available()) {
        if (!server) { int st;waitpid(pid,&st,0);puts("SKIP: no usable RDMA device; segmented gate NOTRUN"); }
        return 0;
    }
    close(sv[server?0:1]);struct efs_conn *c=setup(sv[server?1:0],server);
    if (!strcmp(mode,"interrupt")) {
        if (server) {
            unsigned n=16*1024*1024;unsigned char *p=calloc(1,n);OK(p);
            OK(efs_conn_send_msg(c,42,p,n)!=0);free(p);
        } else {
            /* Consume a prefix without exposing a complete message. */
            for(unsigned i=0;i<10000 && !efs_rdma_assembly_bytes();i++) {
                OK(efs_rdma_message_ready(c->rc)==0);usleep(100);
            }
            OK(efs_rdma_assembly_bytes()>0);
        }
        efs_conn_destroy(c);OK(!efs_rdma_assembly_bytes() && !efs_rdma_live_conns());
        if (!server) {int st;OK(waitpid(pid,&st,0)==pid && WIFEXITED(st)&&WEXITSTATUS(st)==0);puts("PASS: interrupted assembly/credit wait releases all ownership");}
        return 0;
    }
    if (!strcmp(mode,"burst") || !strcmp(mode,"burst-asym")) {
        /* Three small requests in flight per round, default or 2/7 pools.
         * When DATA plus CREDIT frames could exceed a peer's posted
         * receives, the NIC RNR-retried the overflow 0.64 ms later with no
         * completion to wake either side: rounds took ~2 ms, not ~30 us. */
        enum { ROUNDS=400, DEPTH=3 };
        if (server) {
            for (;;) {
                int chan=efs_conn_wait_request(c);if (chan<0) break;
                c->recv_chan=chan;
                uint8_t t;void *p=NULL;uint32_t n=0;
                if (efs_conn_recv_msg(c,&t,&p,&n)) break;
                free(p);uint8_t status=0;OK(!efs_conn_send_msg(c,44,&status,1));
            }
            efs_conn_destroy(c);return 0;
        }
        uint8_t req[48]={0};struct timespec a,b;
        clock_gettime(CLOCK_MONOTONIC,&a);
        for (unsigned r=0;r<ROUNDS;r++) {
            for (unsigned i=0;i<DEPTH;i++) OK(!efs_conn_send_msg(c,42,req,sizeof(req)));
            for (unsigned i=0;i<DEPTH;i++) {
                uint8_t t;void *p=NULL;uint32_t n=0;
                OK(!efs_conn_recv_msg(c,&t,&p,&n) && t==44);free(p);
            }
        }
        clock_gettime(CLOCK_MONOTONIC,&b);
        double ms=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;
        efs_conn_destroy(c);
        int st;OK(waitpid(pid,&st,0)==pid && WIFEXITED(st)&&WEXITSTATUS(st)==0);
        printf("%s: %u rounds of %u requests in %.1f ms (%.1f us/round)\n",mode,ROUNDS,DEPTH,ms,1e3*ms/ROUNDS);
        OK(ms<250.0); /* RNR-retried rounds took ~2 ms each, ~800 ms in total */
        puts("PASS: pipelined small requests never wait out an RNR retry");
        return 0;
    }
    if (!strcmp(mode,"tcp-violation")) {
        if (server) {uint8_t byte=99;OK(write(c->fd,&byte,1)==1);usleep(100000);}
        else {
            uint8_t t;void *p=NULL;uint32_t n;
            OK(efs_conn_recv_msg(c,&t,&p,&n)!=0 && p==NULL);
            struct efs_rdma_message_stats stats;efs_rdma_message_stats(c->rc,&stats);
            OK(stats.tcp_app_rx>0);
        }
        efs_conn_destroy(c);OK(!efs_rdma_assembly_bytes() && !efs_rdma_live_conns());
        if (!server) {int st;OK(waitpid(pid,&st,0)==pid && WIFEXITED(st)&&WEXITSTATUS(st)==0);puts("PASS: TCP application byte after setup retires RDMA connection");}
        return 0;
    }
    if (!strcmp(mode,"deadline")) {
        if (server) usleep(300000); /* no owner progress */
        else {
            struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
            uint64_t begin=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
            efs_net_set_deadline_ms(begin+50);
            unsigned n=16*1024*1024;unsigned char *p=calloc(1,n);OK(p);
            OK(efs_conn_send_msg(c,42,p,n)!=0);free(p);efs_net_set_deadline_ms(0);
            clock_gettime(CLOCK_MONOTONIC,&ts);
            uint64_t end=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
            OK(end-begin<200); /* no per-segment five-second restart */
        }
        efs_conn_destroy(c);OK(!efs_rdma_assembly_bytes() && !efs_rdma_live_conns());
        if (!server) {int st;OK(waitpid(pid,&st,0)==pid && WIFEXITED(st)&&WEXITSTATUS(st)==0);puts("PASS: original 50ms budget covers segment/credit waits");}
        return 0;
    }
    duplex(c,server);
    if (server) {
        for (;;) {
            int chan=efs_conn_wait_request(c);if (chan<0) break;
            OK(chan==EFS_CONN_RDMA);c->recv_chan=chan;
            uint8_t t;void *p=NULL;uint32_t n=0;
            if (efs_conn_recv_msg(c,&t,&p,&n)) break;
            OK(n==4);uint32_t requested;memcpy(&requested,p,4);free(p);
            if (t==43) {
                uint8_t status=7;OK(!efs_conn_send_msg(c,44,&status,1));continue;
            }
            unsigned char *reply=malloc(requested+4);OK(reply);
            reply[0]=0;reply[1]=1;reply[2]=2;reply[3]=3;pattern(reply+4,requested,41);
            OK(!efs_conn_send_msg_parts(c,44,reply,4,reply+4,requested));
            memset(reply,0,requested+4);free(reply);
        }
        struct efs_rdma_message_stats stats;efs_rdma_message_stats(c->rc,&stats);
        OK(stats.data_frames_tx>stats.app_tx && stats.tcp_app_rx==0);
        efs_conn_destroy(c);OK(efs_rdma_live_conns()==0);return 0;
    }
    const unsigned sizes[]={86016-32-5-4-1,86016-32-5-4,86016-32-5-4+1,
        128*1024,1024*1024,16*1024*1024,64*1024*1024};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) {
        unsigned n=sizes[i];unsigned char *buf=malloc(n+2);OK(buf);memset(buf,0xaa,n+2);
        uint8_t hdr[3]={0},status=0xff,t;
        OK(!efs_conn_send_msg(c,42,&n,4));
        OK(!efs_conn_recv_msg_into(c,&t,&status,hdr,3,buf+1,n));
        OK(t==44 && status==0 && hdr[0]==1 && hdr[1]==2 && hdr[2]==3);
        OK(buf[0]==0xaa && buf[n+1]==0xaa);verify(buf+1,n,41);free(buf);
    }
    unsigned n=8;uint8_t body[8],hdr[3],status,t;memset(body,0xab,8);memset(hdr,0xcd,3);
    OK(!efs_conn_send_msg(c,43,&n,4));
    OK(!efs_conn_recv_msg_into(c,&t,&status,hdr,3,body,8));OK(status==7);
    for(unsigned i=0;i<8;i++) OK(body[i]==0xab);
    for(unsigned i=0;i<3;i++) OK(hdr[i]==0xcd);
    struct efs_rdma_message_stats stats;efs_rdma_message_stats(c->rc,&stats);
    OK(stats.data_frames_tx>stats.app_tx && stats.data_frames_rx>stats.app_rx);
    OK(stats.tcp_app_rx==0 && stats.assembly_bytes==0 && stats.global_assembly_bytes==0);
    check_tcp_empty(c->fd);efs_conn_destroy(c);OK(efs_rdma_live_conns()==0);
    int st;OK(waitpid(pid,&st,0)==pid && WIFEXITED(st) && WEXITSTATUS(st)==0);
    printf("PASS: segmented Soft-RoCE duplex8MiB pools2/7, small-request/64MiB-reply, canaries/status errors, source reuse; txframes=%llu rxframes=%llu app=%llu/%llu no-app-TCP\n",
        (unsigned long long)stats.data_frames_tx,(unsigned long long)stats.data_frames_rx,
        (unsigned long long)stats.app_tx,(unsigned long long)stats.app_rx);
    return 0;
}
