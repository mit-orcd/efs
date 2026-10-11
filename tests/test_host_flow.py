#!/usr/bin/env python3
"""Compile production W97 host helpers and sender loop against async transport.
The mock transport routes independently ordered TCP/RDMA replies; production
admission/retirement and queue selection remain the actual source functions.
"""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
    starts=[f'static void *{name}(',f'static void {name}(',f'static int {name}(',f'int {name}(']
    a=next(s.index(x) for x in starts if x in s)
    return s[a:s.index('\n}',a)+2]
def struct(name):
    a=s.index('struct '+name+' {')
    return s[a:s.index('\n};',a)+3]
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "efs/raft_flow.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#undef BO_ON
#undef BO_COUNT
#define BO_ON() 0
#define BO_COUNT(k) ((void)0)
#define HOST_NGROUPS 2
#define HOST_INBOX_MAX 256
#define HOST_INBOX_BYTES (32ull<<20)
#define HOST_FRAME_MAX (4u*1024*1024+92)
#define HOST_OUTBOX_MAX 2048
#define HOST_SENDER_PIPE_MAX 8
#define HOST_SEND_IO_MS 250
#define EFS_CONN_RDMA 1
#define EFS_CONN_TCP 2
struct efs_conn {int kind,fd,recv_chan,last_recv_chan;void *rc;};
uint64_t efs_batch_obs_now_us(void) {
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
 return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static uint64_t now_us_(void) {return efs_batch_obs_now_us();}
static int raft_obs_on(void) {return 1;}
'''+struct('host_inbox_item')+'\n'+struct('host_outbox_item')+'\n'+struct('host_outbox')+r'''
struct efs_raft_host {
 pthread_mutex_t inbox_mu,outbox_mu;
 int running,tx_running,flow_enabled,raft_id,inbox_n,inbox_pressure;
 unsigned inbox_work_n;
 uint64_t inbox_work_bytes,inbox_bytes,inbox_bytes_hi,inbox_drop;
 uint64_t inbox_probe_sent[2][7],inbox_probe_read[2][7];
 struct host_inbox_item inbox[256];
 struct host_outbox tx[7];
};
static struct efs_raft_host *g_host;
static void host_pump_kick(struct efs_raft_host *h) {(void)h;}
static int outbox_nentries(const uint8_t*p,uint32_t n) {return n<80?-1:(int)efs_raft_flow_rd32(p+76);}
'''+function('server_raft_host_inbox')+'\n'+function('host_inbox_retire')+r'''
static int peer_addr(struct efs_raft_host*h,int peer,char*host,size_t cap,uint16_t*port)
{(void)h;(void)peer;snprintf(host,cap,"fixture");*port=1;return 0;}
static struct efs_conn *server_peer_conn_new(const char*h,uint16_t port)
{(void)h;(void)port;struct efs_conn*c=calloc(1,sizeof(*c));assert(c);c->kind=EFS_CONN_RDMA;c->fd=-1;return c;}
static int efs_rdma_conn_nrecv(void*p) {(void)p;return 16;}
static void efs_conn_set_recv_timeout(struct efs_conn*c,int ms) {(void)c;(void)ms;}
static void efs_set_send_timeout(int fd,int ms) {(void)fd;(void)ms;}
struct reply {uint8_t data[8];int channel;unsigned legacy;};
static struct reply outstanding[8];
static int nout;
static unsigned sends,receives,critical,probes,connections,destroyed,malformed,legacy,admitted,lost_reply;
static uint64_t critical_time;
static void efs_conn_destroy(struct efs_conn*c) {nout=0;destroyed++;free(c);}
int efs_conn_send_msg(struct efs_conn*c,uint8_t type,const void*buf,uint32_t len)
{
 const uint8_t*p=buf;struct efs_raft_flow_reply feedback;
 assert(type==EFS_MSG_RAFT);assert(nout<8);
 int rc=server_raft_host_inbox(p,len,&feedback);
 feedback.status=rc==EFS_OK?EFS_RAFT_FLOW_OK:rc==EFS_ERR_BUSY?EFS_RAFT_FLOW_BUSY:EFS_RAFT_FLOW_ERROR;
 struct reply*r=&outstanding[nout++];efs_raft_flow_encode(r->data,&feedback);
 r->channel=len>80?EFS_CONN_TCP:EFS_CONN_RDMA;r->legacy=legacy;
 if(malformed)r->data[2]=99;
 c->recv_chan=r->channel;__atomic_add_fetch(&sends,1,__ATOMIC_RELAXED);
 if(rc==EFS_OK)__atomic_add_fetch(&admitted,1,__ATOMIC_RELAXED);
 if(efs_raft_flow_critical(p,len)){__atomic_add_fetch(&critical,1,__ATOMIC_RELAXED);__atomic_store_n(&critical_time,efs_batch_obs_now_us(),__ATOMIC_RELAXED);}
 if(efs_raft_flow_empty(p,len)){probes++;assert(p[2]==1);assert(!efs_raft_flow_rd64(p+44));assert(!efs_raft_flow_rd64(p+52));}
 return 0;
}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*type,void**buf,uint32_t*len)
{
 int i;assert(nout>0);
 if(lost_reply){lost_reply=0;return -1;}
 /* RDMA replies are drained before TCP even if a TCP request went first. */
 for(i=0;i<nout;i++)if(outstanding[i].channel==c->recv_chan)break;
 assert(i<nout);struct reply r=outstanding[i];
 memmove(outstanding+i,outstanding+i+1,(size_t)(--nout-i)*sizeof(r));
 *type=EFS_MSG_RAFT_REPLY;*len=r.legacy?0:8;*buf=*len?malloc(8):NULL;
 if(*len)memcpy(*buf,r.data,8);c->last_recv_chan=r.channel;__atomic_add_fetch(&receives,1,__ATOMIC_RELAXED);return 0;
}
'''+function('host_send_ready')+'\n'+function('host_sender_depth')+'\n'+function('host_sender_take')+'\n'+function('host_sender_pick')+'\n'+function('host_sender_wait')+'\n'+function('host_sender')+r'''
static void wr32(uint8_t*p,unsigned x){p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static uint8_t *frame(unsigned type,unsigned group,unsigned data,uint32_t*len)
{
 *len=data?93:80;uint8_t*p=calloc(1,*len);assert(p);p[0]=type;p[1]=group;
 wr32(p+4,0);wr32(p+8,1);p[19]=1;p[27]=1;p[35]=7; /* term/boot/read */
 if(data){wr32(p+76,1);p[87]=1;wr32(p+88,1);p[92]='a';}return p;
}
static struct efs_raft_host *host(void)
{
 struct efs_raft_host*h=calloc(1,sizeof(*h));assert(h);
 pthread_mutex_init(&h->inbox_mu,NULL);pthread_mutex_init(&h->outbox_mu,NULL);
 h->flow_enabled=h->running=h->tx_running=1;h->raft_id=1;
 pthread_condattr_t attr;pthread_condattr_init(&attr);
#ifdef __linux__
 pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
#endif
 for(int i=0;i<7;i++){h->tx[i].h=h;h->tx[i].peer=i;pthread_cond_init(&h->tx[i].cv,&attr);}
 pthread_condattr_destroy(&attr);return h;
}
static void retire(struct efs_raft_host*h)
{
 unsigned n=h->inbox_n;uint64_t bytes=h->inbox_bytes;
 for(unsigned i=0;i<n;i++)free(h->inbox[i].buf);
 h->inbox_n=0;h->inbox_bytes=0;if(h->flow_enabled)host_inbox_retire(h,n,bytes);
}
static void queue(struct host_outbox*tx,unsigned type,unsigned group,unsigned data)
{
 uint32_t len;uint8_t*p=frame(type,group,data,&len);
 pthread_mutex_lock(&tx->h->outbox_mu);
 struct host_outbox_item*q=data?tx->ent:tx->pri;int*n=data?&tx->nent:&tx->npri;
 assert(*n<2048);q[*n]=(struct host_outbox_item){p,len};(*n)++;tx->queued_bytes+=len;
 int gi=efs_raft_flow_group(group);if(data)tx->pending_entries[gi]++;
 if(type==EFS_RAFT_MSG_AE_REQ){memcpy(tx->probe_header[gi],p,80);tx->probe_valid[gi]=1;}
 pthread_cond_signal(&tx->cv);pthread_mutex_unlock(&tx->h->outbox_mu);
}
static void codec(void)
{
 assert(!efs_raft_flow_probe_due(10000,1,9999,2));
 assert(!efs_raft_flow_probe_due(10000,1,14999,2));
 assert(efs_raft_flow_probe_due(10000,1,15000,2));
 assert(!efs_raft_flow_probe_due(10000,1,59999,1));
 assert(efs_raft_flow_probe_due(10000,1,60000,1));
 uint8_t wire[8];struct efs_raft_flow_reply r={1,1,500},d;efs_raft_flow_encode(wire,&r);
 assert(!efs_raft_flow_decode(wire,8,&d)&&d.status==1&&d.retry_ms==500);
 assert(efs_raft_flow_decode(NULL,0,&d)==1);
 for(unsigned i=0;i<8;i++)assert(efs_raft_flow_decode(wire,i?i:9,&d)<0);
 wire[2]=9;assert(efs_raft_flow_decode(wire,8,&d)<0);
 struct efs_raft_flow_sender st={0};efs_raft_flow_feedback(&st,&r,100);
 assert(st.active&&st.delay_ms==50&&st.deadline_us<=50100);
 uint64_t deadline=st.deadline_us;efs_raft_flow_feedback(&st,&r,deadline-1);
 assert(st.deadline_us==deadline); /* heartbeats cannot postpone data forever */
 r=(struct efs_raft_flow_reply){0,0,0};for(int i=0;i<24;i++)efs_raft_flow_feedback(&st,&r,100000);
 assert(!st.active&&st.window==8);
}
static void admission(void)
{
 struct efs_raft_host*h=host();g_host=h;struct efs_raft_flow_reply f;
 uint32_t len;uint8_t*p=frame(3,0,1,&len);
 for(int i=0;i<64;i++)assert(server_raft_host_inbox(p,len,&f)==EFS_OK);
 assert(f.pressure&&h->inbox_work_n==64);
 assert(server_raft_host_inbox(p,len,&f)==EFS_ERR_BUSY);
 /* Detached accepted work remains charged until processing AND KV durability. */
 unsigned n=h->inbox_n;uint64_t bytes=h->inbox_bytes;
 for(unsigned i=0;i<n;i++)free(h->inbox[i].buf);h->inbox_n=0;h->inbox_bytes=0;
 assert(server_raft_host_inbox(p,len,&f)==EFS_ERR_BUSY);
 host_inbox_retire(h,n,bytes);assert(!h->inbox_pressure&&!h->inbox_work_n);
 assert(server_raft_host_inbox(p,len,&f)==EFS_OK);retire(h);free(p);
 /* High water admits replies/votes, but hard caps cannot be bypassed. */
 h->inbox_pressure=1;p=frame(4,2,0,&len);
 for(int i=0;i<256;i++)assert(server_raft_host_inbox(p,len,&f)==EFS_OK);
 assert(server_raft_host_inbox(p,len,&f)==EFS_ERR_BUSY);retire(h);free(p);
 /* One empty reserve probe per sender/group, not arbitrary empty flooding. */
 h->inbox_pressure=1;p=frame(3,0,0,&len);
 assert(server_raft_host_inbox(p,len,&f)==EFS_OK);
 assert(server_raft_host_inbox(p,len,&f)==EFS_ERR_BUSY);
 p[1]=2;assert(server_raft_host_inbox(p,len,&f)==EFS_OK);
 p[1]=1;assert(server_raft_host_inbox(p,len,&f)==EFS_ERR_PROTO);
 p[1]=0;wr32(p+76,1);assert(server_raft_host_inbox(p,len,&f)==EFS_ERR_PROTO);
 retire(h);free(p);
 h->flow_enabled=0;p=frame(3,0,1,&len);
 assert(server_raft_host_inbox(p,len,&f)==EFS_OK&&f.status==3&&!h->inbox_work_n);
 retire(h);free(p);free(h);
}
static void selection(void)
{
 struct efs_raft_host*h=host();struct host_outbox*tx=&h->tx[1];
 tx->flow=(struct efs_raft_flow_sender){.active=1,.window=1,.delay_ms=50,.deadline_us=100000};
 queue(tx,3,0,1);queue(tx,3,2,1);queue(tx,4,0,0);queue(tx,2,2,0);
 assert(!host_send_ready(h,1,0)&&!host_send_ready(h,1,2));
 uint8_t*b[8];uint32_t n[8];int count=host_sender_pick(tx,b,n,8,100);
 assert(count==4&&tx->nent==2);assert(b[0][0]==4&&b[3][0]==2);
 for(int i=0;i<count;i++)free(b[i]);
 assert(!host_sender_pick(tx,b,n,8,200));
 /* New read context can escape in 5ms; different groups never overwrite. */
 tx->probe_header[0][35]=8;count=host_sender_pick(tx,b,n,8,5100);
 assert(count==1&&efs_raft_flow_rd64(b[0]+28)==8);free(b[0]);
 count=host_sender_pick(tx,b,n,1,100000);assert(count==1&&n[0]==80);free(b[0]);
 count=host_sender_pick(tx,b,n,1,100000);assert(count==1&&n[0]>80);free(b[0]);
 assert(host_send_ready(h,1,0)&&!host_send_ready(h,1,2));
 for(int i=0;i<tx->nent;i++)free(tx->ent[i].buf);free(h);
}
/* Slow single-slot round trips must not starve control or either group's
 * fresh read probe behind an always-due entry stream. This also exercises
 * the scheduler independently of the transport's usual eight-slot TCP lane. */
static void slow_lane_fairness(void)
{
 struct efs_raft_host*h=host();struct host_outbox*tx=&h->tx[1];
 tx->flow=(struct efs_raft_flow_sender){.active=1,.window=1,.delay_ms=50,.deadline_us=1};
 for(int i=0;i<8;i++){queue(tx,3,(i&1)?2:0,1);queue(tx,4,(i&1)?2:0,0);}
 unsigned critical_seen=0,data_seen=0,probe_seen[2]={0};
 uint8_t*b[8];uint32_t n[8];
 for(unsigned round=0;round<4;round++) {
  int count=host_sender_pick(tx,b,n,1,100000ull*(round+1));assert(count==1);
  if(efs_raft_flow_critical(b[0],n[0]))critical_seen++;
  else if(n[0]>80)data_seen++;
  else probe_seen[efs_raft_flow_group(b[0][1])]++;
  free(b[0]);
 }
 assert(critical_seen && data_seen && probe_seen[0] && probe_seen[1]);
 for(int i=0;i<tx->npri;i++)free(tx->pri[i].buf);
 for(int i=0;i<tx->nent;i++)free(tx->ent[i].buf);free(h);
}
#ifdef __linux__
static unsigned pump_stop;
static void *pump_worker(void *arg)
{
 struct efs_raft_host*h=arg;
 while(!__atomic_load_n(&pump_stop,__ATOMIC_RELAXED)) {
  pthread_mutex_lock(&h->inbox_mu);
  unsigned n=h->inbox_n;uint64_t bytes=h->inbox_bytes;
  for(unsigned i=0;i<n;i++)free(h->inbox[i].buf);
  h->inbox_n=0;h->inbox_bytes=0;pthread_mutex_unlock(&h->inbox_mu);
  if(n)host_inbox_retire(h,n,bytes);usleep(2000);
 }
 return NULL;
}
static void stop_sender(struct efs_raft_host*h,struct host_outbox*tx,pthread_t tid)
{
 pthread_mutex_lock(&h->outbox_mu);h->tx_running=0;pthread_cond_signal(&tx->cv);pthread_mutex_unlock(&h->outbox_mu);
 assert(!pthread_join(tid,NULL));assert(!nout);assert(!tx->queued_bytes);
}
static void asynchronous(void)
{
 struct efs_raft_host*h=host();g_host=host();struct host_outbox*tx=&h->tx[1];
 /* Force receiver pressure and a real mixed-channel pipeline. */
 g_host->inbox_pressure=1;g_host->inbox_work_n=64;
 queue(tx,3,0,1);queue(tx,3,2,1);queue(tx,4,0,0);
 tx->conn=server_peer_conn_new("fixture",1);connections++;
 pthread_t tid;assert(!pthread_create(&tid,NULL,host_sender,tx));usleep(20000);
 pthread_mutex_lock(&h->outbox_mu);assert(tx->flow.active);pthread_mutex_unlock(&h->outbox_mu);
 assert(__atomic_load_n(&sends,__ATOMIC_RELAXED)<30); /* no busy-spin / unbounded retransmit */
 uint64_t begin=efs_batch_obs_now_us();queue(tx,2,2,0);usleep(10000);
 assert(__atomic_load_n(&critical,__ATOMIC_RELAXED)>=2&&__atomic_load_n(&critical_time,__ATOMIC_RELAXED)-begin<15000);
 /* Pressure recovery uses the actual retirement helper on another thread. */
 pthread_mutex_lock(&g_host->inbox_mu);g_host->inbox_work_n-=64;pthread_mutex_unlock(&g_host->inbox_mu);
 pthread_t pump;assert(!pthread_create(&pump,NULL,pump_worker,g_host));
 for(int i=0;i<48;i++)queue(tx,3,(i&1)?2:0,1);
 int recovered=0;for(int i=0;i<400;i++){
  usleep(5000);pthread_mutex_lock(&h->outbox_mu);
  recovered=!tx->flow.active&&!tx->nent;
  pthread_mutex_unlock(&h->outbox_mu);if(recovered)break;
 }
 assert(recovered);assert(__atomic_load_n(&admitted,__ATOMIC_RELAXED)>=48);
 __atomic_store_n(&pump_stop,1,__ATOMIC_RELAXED);assert(!pthread_join(pump,NULL));
 /* Stop while pressured: wakeup and join cannot wait for a 50ms sleep. */
 stop_sender(h,tx,tid);assert(destroyed);
 /* Reply order was independently RDMA first, then earlier TCP requests. */
 assert(sends==receives&&probes>=2);retire(g_host);free(g_host);free(h);
 /* Legacy status cannot clear pressure; malformed status reconnects. */
 h=host();g_host=host();tx=&h->tx[1];legacy=1;
 tx->flow=(struct efs_raft_flow_sender){.active=1,.window=1,.delay_ms=50,.deadline_us=efs_batch_obs_now_us()+50000};
 queue(tx,3,0,1);assert(!pthread_create(&tid,NULL,host_sender,tx));usleep(20000);
 pthread_mutex_lock(&h->outbox_mu);assert(tx->flow.active);pthread_mutex_unlock(&h->outbox_mu);
 stop_sender(h,tx,tid);retire(g_host);free(g_host);free(h);legacy=0;
 h=host();g_host=host();tx=&h->tx[1];malformed=1;
 queue(tx,3,0,1);assert(!pthread_create(&tid,NULL,host_sender,tx));usleep(10000);
 stop_sender(h,tx,tid);assert(tx->st_fail);retire(g_host);free(g_host);free(h);malformed=0;
 h=host();g_host=host();tx=&h->tx[1];lost_reply=1;
 queue(tx,3,0,1);queue(tx,3,2,1);assert(!pthread_create(&tid,NULL,host_sender,tx));usleep(20000);
 stop_sender(h,tx,tid);assert(tx->st_fail);retire(g_host);free(g_host);free(h);
}
#endif
int main(void){codec();admission();selection();slow_lane_fairness();
#ifdef __linux__
 asynchronous();
#endif
 puts("host flow: codec, bounded admission/detached work, control/read escape PASS");
#ifdef __linux__
 puts("Linux async sender: mixed-channel BUSY drain, recovery, legacy/malformed/lost replies and shutdown PASS");
#else
 puts("Linux async sender: not run on this platform");
#endif
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-flow-') as d:
    p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
    subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-variable','-I'+str(root/'include'),str(p),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
