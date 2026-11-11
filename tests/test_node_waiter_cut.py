#!/usr/bin/env python3
"""K106 (a): a request waiting for its reply from a halted member is cut, one from a slow live member is not.

Production efs_client_conn_await_reply / node_exchange / release / slot lookup from src/client/node_cache.c and the
request gating from src/client/inode_rpc.c. A waiter that has heard nothing for EFS_NODE_PROBE_MS runs one real
exchange (one waiter per node at a time); silence for the whole bound marks the member down and shuts down the
conns of every waiter whose reply is not already readable, so their receive fails and the callers' retry runs; an
answered exchange leaves the wait alone. The stub declaration blocks on a semaphore the test releases, so no sleep
and no polling: a missed cut hangs the waiter and the alarm fails the test. EFS_NODE_PROBE_MS is scaled to 40 ms
and EFS_IO_TIMEOUT_MS to 300 ms.
Usage: test_node_waiter_cut.py halted|slow|two|noreply|deadline|race|gating (one scenario each):
 halted   the peer sends nothing, its declaration is never answered: the waiter's conn is cut (a transport error);
 slow     the declaration is answered, the original reply comes later: not cut, completes, no node-waiter-cut line;
 two      two waiters on one node: one exchange runs, both get the verdict;
 noreply  every declaration is answered, the reply never comes, no operation deadline: the wait ends at the conn's
          recv bound and the conn is shut down so the receive fails at once;
 deadline a real operation deadline 20 s away and a halted member: the wait ends at the exchange verdict;
 race     two waiters, the exchange runs, one reply is readable before the cut: that waiter is not shut down and
          keeps its pool slot; the other is cut and its conn does not return to the pool;
 gating   which request types reach the hook: reads and the four directory mutations with an op-id, nothing else."""
from pathlib import Path
import os,shlex,subprocess,sys,tempfile
sys.path.insert(0,str(Path(__file__).resolve().parent));from sanitize_env import cflags,wrap
case=sys.argv[1];assert case in('halted','slow','two','noreply','deadline','race','gating'),case
root=Path(__file__).resolve().parents[1];s=(root/'src/client/node_cache.c').read_text();r=(root/'src/client/inode_rpc.c').read_text()
def cut(t,x,y):return t[t.index(x):t.index(y)]
CC=shlex.split(os.environ.get('CC','cc'))
def build(d,code):
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(CC+['-std=gnu11','-pthread']+cflags()+['-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 return str(p.with_suffix(''))
if case=='gating':
 code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
'''+cut(r,'static int rpc_identified(','static const char *rpc_req_ident(')+cut(r,'static int rpc_type_mutates(','static int rpc_send_recv_shard_once(')+r'''
static const uint8_t held[]={EFS_MSG_INODE_SETATTR,EFS_MSG_INODE_APPEND,EFS_MSG_INODE_HOLD,EFS_MSG_REPORT_CHUNKS};
int main(void){
 uint8_t buf[512];memset(buf,0,sizeof buf);
 /* a plain request of every type: only the mutating types are not cut; the four directory ops need their op-id */
 for(int t=0;t<256;t++){
  int mut=0;for(unsigned i=0;i<sizeof held;i++)mut|=t==held[i];
  mut|=t==EFS_MSG_INODE_CREATE||t==EFS_MSG_INODE_UNLINK||t==EFS_MSG_INODE_LINK||t==EFS_MSG_INODE_RENAME_AT||t==EFS_MSG_CREATE_RETAINED||t==EFS_MSG_INODE_FLOCK||t==EFS_MSG_XATTR;
  if(t==EFS_MSG_CREATE_RETAINED||t==EFS_MSG_INODE_FLOCK||t==EFS_MSG_XATTR)continue; /* body decides, below */
  if(rpc_reply_wait_cut((uint8_t)t,buf,16)!=!mut){fprintf(stderr,"type %d cut=%d\n",t,rpc_reply_wait_cut((uint8_t)t,buf,16));return 1;}
 }
 struct{uint8_t t;uint32_t len;}id[]={{EFS_MSG_INODE_CREATE,sizeof(struct efs_msg_inode_create)},{EFS_MSG_INODE_UNLINK,sizeof(struct efs_msg_inode_unlink)},
  {EFS_MSG_INODE_LINK,sizeof(struct efs_msg_inode_link)},{EFS_MSG_INODE_RENAME_AT,sizeof(struct efs_msg_inode_rename_at)}};
 for(unsigned i=0;i<4;i++){
  assert(rpc_reply_wait_cut(id[i].t,buf,id[i].len+EFS_OPID_WIRE_LEN));  /* with its op-id: cut */
  assert(!rpc_reply_wait_cut(id[i].t,buf,id[i].len));                  /* without: waits as before */
 }
 struct efs_msg_inode_flock fl;memset(&fl,0,sizeof fl);
 fl.op=EFS_FLOCK_GETLK;assert(rpc_reply_wait_cut(EFS_MSG_INODE_FLOCK,&fl,sizeof fl));
 fl.op=EFS_FLOCK_SH;assert(!rpc_reply_wait_cut(EFS_MSG_INODE_FLOCK,&fl,sizeof fl));
 struct efs_msg_xattr xa;memset(&xa,0,sizeof xa);
 xa.op=EFS_XATTR_GET;assert(rpc_reply_wait_cut(EFS_MSG_XATTR,&xa,sizeof xa));
 xa.op=EFS_XATTR_SET;assert(!rpc_reply_wait_cut(EFS_MSG_XATTR,&xa,sizeof xa));
 xa.op=EFS_XATTR_REMOVE;assert(!rpc_reply_wait_cut(EFS_MSG_XATTR,&xa,sizeof xa));
 struct efs_msg_create_retained cr;memset(&cr,0,sizeof cr);
 cr.probe=0;assert(!rpc_reply_wait_cut(EFS_MSG_CREATE_RETAINED,&cr,sizeof cr));
 cr.probe=1;assert(rpc_reply_wait_cut(EFS_MSG_CREATE_RETAINED,&cr,sizeof cr));
 puts("client waiter cut: scenario PASS");return 0;}
'''
 with tempfile.TemporaryDirectory(prefix='efs-node-waiter-gate-') as d:
  x=build(d,code);q=subprocess.run(wrap()+[x],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=90)
  print(q.stdout,end='');assert q.returncode==0,(q.returncode,q.stderr)
 sys.exit(0)
a=s.index('enum { NODE_EX_FAILED');b=s.index('struct efs_conn *efs_client_conn_get(')
code=r'''
#include "client_internal.h"
#include "efs/network.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct efs_client g_client;
static int64_t monotonic_ms(void);
enum{SILENT,ANSWER};static int mode,declares,gated;static int64_t deadline_abs;
static sem_t entered,go;static pthread_t runner;
#define DECLARES __atomic_load_n(&declares,__ATOMIC_SEQ_CST)
static int peer[2];
int efs_connect_tcp(const char *h,uint16_t p){assert(!strcmp(h,"10.0.0.2")&&p==1234);assert(!socketpair(AF_UNIX,SOCK_STREAM,0,peer));return peer[0];}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)server;return (struct efs_conn *)(intptr_t)(fd+1);}
static int destroyed;static void *destroyed_conn;
void efs_conn_destroy(struct efs_conn *c){if((uintptr_t)c<65536){close((int)(intptr_t)c-1);close(peer[1]);}else{destroyed++;destroyed_conn=c;}}
uint64_t efs_net_deadline_ms(void){return (uint64_t)deadline_abs;}
static __thread int silent_once; /* the exchange's own bound has run out */
int efs_net_remaining_ms(int cap){if(silent_once){silent_once=0;return 0;}if(!deadline_abs)return cap;int64_t r=deadline_abs-monotonic_ms();return r<=0?0:r<cap?(int)r:cap;}
int efs_conn_declare(struct efs_conn *c){
 (void)c;__atomic_fetch_add(&declares,1,__ATOMIC_SEQ_CST);runner=pthread_self();
 if(gated){sem_post(&entered);while(sem_wait(&go)&&errno==EINTR);}
 if(mode==ANSWER)return EFS_OK;
 silent_once=1;return EFS_ERR_NET;}
uint64_t efs_client_rpc_deadline_ms(void){return 0;}
void efs_client_rpc_set_deadline_ms(uint64_t d){(void)d;}
#define EFS_NODE_PROBE_MS 40
#undef EFS_IO_TIMEOUT_MS
#define EFS_IO_TIMEOUT_MS 300
static void node_probe_maybe_start(int idx, int64_t now);
#define EFS_NODE_DOWN_MS 30000
#define EFS_NODE_ADMISSION_MS 5000
#define EFS_NODE_DOWN_FAILS 4
static int64_t monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
'''+cut(s,'static int pool_size(void)','void efs_client_conn_init(void)')+cut(s,'#define EFS_CLIENT_CONN_IDLE_MS','/* K33 live client view')+s[a:b]+cut(s,'/* conn_wait: 0 no wait','void efs_client_conn_invalidate_node')+cut(s,'void efs_client_conn_await_reply(','static char g_shutdown_reason')+r'''
struct W{struct efs_conn *c;int sv[2];int done;pthread_t t;};
static void *waiter(void *p){struct W *w=p;efs_client_conn_await_reply(3,w->c);__atomic_store_n(&w->done,1,__ATOMIC_SEQ_CST);return NULL;}
static void start(struct W *w,int slot){assert(!socketpair(AF_UNIX,SOCK_STREAM,0,w->sv));w->c=calloc(1,sizeof(*w->c));w->c->fd=w->sv[0];w->c->rc=NULL;
 w->done=0;g_client.conn[0][slot]=w->c;g_client.conn_busy[0][slot]=1;assert(!pthread_create(&w->t,NULL,waiter,w));}
static void reset(void){
 g_client.node_exch_end_ms[0]=0;memset(g_client.node_mute,0,sizeof g_client.node_mute);g_client.node_fail_streak[0]=0;g_client.node_down_until_ms[0]=0;
 declares=0;}
static void finish(struct W *w,int slot){close(w->sv[0]);close(w->sv[1]);free(w->c);g_client.conn[0][slot]=NULL;g_client.conn_busy[0][slot]=0;}
int main(int argc,char **argv){
 (void)argc;const char *which=argv[1];
 alarm(60);sem_init(&entered,0,0);sem_init(&go,0,0);
 for(int i=0;i<EFS_MAX_NODES;i++){pthread_mutex_init(&g_client.conn_lock[i],NULL);pthread_cond_init(&g_client.conn_cv[i],NULL);g_client.node_probe_fd[i]=-1;}
 strcpy(g_client.nodes[0].addr,"10.0.0.2");g_client.nodes[0].port=1234;g_client.nodes[0].id=3;g_client.node_count=1;
 char x;struct W w[2];reset();
 if(!strcmp(which,"halted")){
  /* The halted member: the peer sends nothing and its declaration is never answered. The waiter marks it down and
   * its own conn is shut down, so the receive that follows fails (EOF) instead of waiting out the transport timeout. */
  mode=SILENT;start(&w[0],0);pthread_join(w[0].t,NULL);
  assert(DECLARES==1&&g_client.node_mute[0]&&g_client.node_down_until_ms[0]>monotonic_ms()+EFS_NODE_DOWN_MS-1000&&g_client.node_fail_streak[0]==EFS_NODE_DOWN_FAILS);
  assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)==0);                 /* shut down: the transport error of the retry path */
  finish(&w[0],0);
 }else if(!strcmp(which,"slow")){
  /* A slow but live member: its declaration is answered, the original reply comes later. Not cut, and it completes. */
  mode=ANSWER;gated=1;start(&w[0],0);
  sem_wait(&entered);sem_post(&go);                              /* the exchange ran and was answered */
  assert(!__atomic_load_n(&w[0].done,__ATOMIC_SEQ_CST));
  assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)<0&&errno==EAGAIN);   /* still open, still waiting */
  assert(write(w[0].sv[1],"r",1)==1);pthread_join(w[0].t,NULL);
  assert(recv(w[0].sv[0],&x,1,0)==1&&x=='r'&&!g_client.node_down_until_ms[0]&&!g_client.node_mute[0]&&!g_client.conn_wait[0][0]);
  finish(&w[0],0);
 }else if(!strcmp(which,"two")){
  /* Two waiters on one node, the exchange held open: one exchange runs (the other keeps waiting for its verdict),
   * both get the verdict and both conns are shut down. */
  mode=SILENT;gated=1;start(&w[0],0);start(&w[1],1);
  sem_wait(&entered);sem_post(&go);
  pthread_join(w[0].t,NULL);pthread_join(w[1].t,NULL);
  assert(DECLARES==1&&g_client.node_mute[0]&&g_client.node_down_until_ms[0]>0);
  assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)==0&&recv(w[1].sv[0],&x,1,MSG_DONTWAIT)==0);
  finish(&w[0],0);finish(&w[1],1);
 }else if(!strcmp(which,"deadline")){
  /* A real operation deadline 20 s away and a halted member: the verdict ends the wait, not the deadline. */
  mode=SILENT;deadline_abs=monotonic_ms()+20000;start(&w[0],0);pthread_join(w[0].t,NULL);
  assert(monotonic_ms()<deadline_abs-10000&&DECLARES==1&&g_client.node_mute[0]&&g_client.node_down_until_ms[0]>0);
  assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)==0);finish(&w[0],0);
 }else if(!strcmp(which,"race")){
  /* Two waiters, the exchange held open. The runner's reply becomes readable before the verdict: it is not shut down,
   * reads its reply and keeps its pool slot on release. The other is cut and its conn never returns to the pool. */
  mode=SILENT;gated=1;start(&w[0],0);start(&w[1],1);
  sem_wait(&entered);                                            /* the runner is inside the exchange */
  int ri=pthread_equal(runner,w[0].t)?0:1,oi=1-ri;
  assert(write(w[ri].sv[1],"r",1)==1);sem_post(&go);
  pthread_join(w[0].t,NULL);pthread_join(w[1].t,NULL);
  assert(DECLARES==1&&g_client.node_down_until_ms[0]>0);
  assert(recv(w[ri].sv[0],&x,1,0)==1&&x=='r'&&recv(w[ri].sv[0],&x,1,MSG_DONTWAIT)<0&&errno==EAGAIN&&g_client.conn_wait[0][ri]==0);
  assert(recv(w[oi].sv[0],&x,1,MSG_DONTWAIT)==0&&g_client.conn_wait[0][oi]==CONN_WAIT_CUT);
  efs_client_conn_release(3,w[ri].c);assert(g_client.conn[0][ri]==w[ri].c&&!g_client.conn_busy[0][ri]&&!destroyed);
  efs_client_conn_release(3,w[oi].c);assert(!g_client.conn[0][oi]&&!g_client.conn_busy[0][oi]&&destroyed==1&&destroyed_conn==w[oi].c&&g_client.conn_wait[0][oi]==0);
  finish(&w[0],0);finish(&w[1],1);
 }else{
  /* A live member that answers every declaration but never sends the reply, no operation deadline: the wait ends
   * at the conn's recv bound with the conn shut down, no cut verdict, the member not marked down. */
  mode=ANSWER;start(&w[0],0);pthread_join(w[0].t,NULL);
  assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)==0&&!g_client.node_down_until_ms[0]&&!g_client.node_mute[0]);
  assert(g_client.conn_wait[0][0]==CONN_WAIT_CUT);               /* shut down at the bound: release would destroy it */
  finish(&w[0],0);
 }
 puts("client waiter cut: scenario PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-node-waiter-cut-') as d:
 x=build(d,code)
 q=subprocess.run(wrap()+[x,case],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=90)
 print(q.stdout,end='');err=q.stderr
 assert q.returncode==0,(q.returncode,err)
cutl=[l for l in err.splitlines() if l.startswith('node-waiter-cut:')]
want={'halted':['conns=1 '],'two':['conns=2 '],'deadline':['conns=1 '],'race':['conns=1 '],'slow':[],'noreply':[]}[case]   # a live member's request is never cut
assert len(cutl)==len(want) and all('node=3 '+w in l for l,w in zip(cutl,want)),cutl
