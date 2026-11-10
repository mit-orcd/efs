#!/usr/bin/env python3
"""K106 (a): a request waiting for its reply from a halted member is cut, one from a slow live member is not.

Production efs_client_conn_await_reply / node_exchange from src/client/node_cache.c. A waiter that has heard
nothing for EFS_NODE_PROBE_MS runs one real exchange (one waiter per node at a time); silence for the whole bound
marks the member down and shuts down the conns of every waiter, so their receive fails and the callers' retry runs;
an answered exchange leaves the wait alone. No wall-clock bound is asserted: a missed cut hangs the waiter and the
alarm fails the test. EFS_NODE_PROBE_MS is scaled to 40 ms so the test does not wait seconds."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/node_cache.c').read_text()
a=s.index('enum { NODE_EX_FAILED');b=s.index('struct efs_conn *efs_client_conn_get(')
c=s.index('void efs_client_conn_await_reply(');d=s.index('static char g_shutdown_reason')
code=r'''
#include "client_internal.h"
#include "efs/network.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct efs_client g_client;
enum{SILENT,ANSWER};static int mode,declares,delay_ms;static __thread int silent_once;
#define DECLARES __atomic_load_n(&declares,__ATOMIC_SEQ_CST)
static int peer[2];
int efs_connect_tcp(const char *h,uint16_t p){assert(!strcmp(h,"10.0.0.2")&&p==1234);assert(!socketpair(AF_UNIX,SOCK_STREAM,0,peer));return peer[0];}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)server;return (struct efs_conn *)(intptr_t)(fd+1);}
void efs_conn_destroy(struct efs_conn *c){close((int)(intptr_t)c-1);close(peer[1]);}
int efs_net_remaining_ms(int cap){if(silent_once){silent_once=0;return 0;}return cap;}
int efs_conn_declare(struct efs_conn *c){(void)c;__atomic_fetch_add(&declares,1,__ATOMIC_SEQ_CST);if(delay_ms)usleep(delay_ms*1000);
 if(mode==ANSWER)return EFS_OK;silent_once=1;return EFS_ERR_NET;}
uint64_t efs_client_rpc_deadline_ms(void){return 0;}
void efs_client_rpc_set_deadline_ms(uint64_t d){(void)d;}
int node_index_for_id(efs_node_id_t id){(void)id;return 0;}
static int pool_size(void){return 4;}
static int slot_for_conn(uint32_t idx,struct efs_conn *conn){for(int s=0;s<pool_size();s++)if(g_client.conn[idx][s]==conn)return s;return -1;}
#define EFS_NODE_PROBE_MS 40
static void node_probe_maybe_start(int idx, int64_t now);
#define EFS_NODE_DOWN_MS 30000
#define EFS_NODE_ADMISSION_MS 5000
#define EFS_NODE_DOWN_FAILS 4
static int64_t monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
'''+s[a:b]+s[c:d]+r'''
struct W{struct efs_conn *c;int sv[2];int done;pthread_t t;};
static void *waiter(void *p){struct W *w=p;efs_client_conn_await_reply(3,w->c);__atomic_store_n(&w->done,1,__ATOMIC_SEQ_CST);return NULL;}
static void start(struct W *w,int slot){assert(!socketpair(AF_UNIX,SOCK_STREAM,0,w->sv));w->c=calloc(1,sizeof(*w->c));w->c->fd=w->sv[0];w->c->rc=NULL;
 w->done=0;g_client.conn[0][slot]=w->c;g_client.conn_busy[0][slot]=1;assert(!pthread_create(&w->t,NULL,waiter,w));}
static void reset(void){memset(g_client.node_mute,0,sizeof g_client.node_mute);g_client.node_fail_streak[0]=0;g_client.node_down_until_ms[0]=0;
 g_client.node_exch_end_ms[0]=0;declares=0;}
static void finish(struct W *w,int slot){close(w->sv[0]);close(w->sv[1]);free(w->c);g_client.conn[0][slot]=NULL;g_client.conn_busy[0][slot]=0;}
int main(void){
 alarm(60);
 for(int i=0;i<EFS_MAX_NODES;i++){pthread_mutex_init(&g_client.conn_lock[i],NULL);pthread_cond_init(&g_client.conn_cv[i],NULL);g_client.node_probe_fd[i]=-1;}
 strcpy(g_client.nodes[0].addr,"10.0.0.2");g_client.nodes[0].port=1234;
 char x;struct W w[2];
 /* 1. The halted member: the peer sends nothing and its declaration is never answered. The waiter marks it down and
  * its own conn is shut down, so the receive that follows fails (EOF) instead of waiting out the transport timeout. */
 fprintf(stderr,"CASE 1\n");reset();mode=SILENT;delay_ms=0;start(&w[0],0);pthread_join(w[0].t,NULL);
 assert(DECLARES==1&&g_client.node_mute[0]&&g_client.node_down_until_ms[0]>monotonic_ms()+EFS_NODE_DOWN_MS-1000&&g_client.node_fail_streak[0]==EFS_NODE_DOWN_FAILS);
 assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)==0);                 /* shut down: the transport error of the retry path */
 assert(!g_client.conn_wait[0][0]);
 finish(&w[0],0);
 /* 2. A slow but live member: its declaration is answered, the original reply comes later. Not cut, and it completes. */
 fprintf(stderr,"CASE 2\n");reset();mode=ANSWER;start(&w[0],0);
 for(int i=0;i<5000&&DECLARES<1;i++)usleep(1000);
 assert(DECLARES>=1&&!__atomic_load_n(&w[0].done,__ATOMIC_SEQ_CST));
 assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)<0&&errno==EAGAIN);   /* still open, still waiting */
 assert(write(w[0].sv[1],"r",1)==1);pthread_join(w[0].t,NULL);
 assert(recv(w[0].sv[0],&x,1,0)==1&&x=='r'&&!g_client.node_down_until_ms[0]&&!g_client.node_mute[0]&&!g_client.conn_wait[0][0]);
 finish(&w[0],0);
 /* 3. Two waiters on one node, the exchange slow: one exchange runs (the other keeps waiting for its verdict),
  * both get the verdict and both conns are shut down. */
 fprintf(stderr,"CASE 3\n");reset();mode=SILENT;delay_ms=200;start(&w[0],0);start(&w[1],1);
 pthread_join(w[0].t,NULL);pthread_join(w[1].t,NULL);
 assert(DECLARES==1&&g_client.node_mute[0]&&g_client.node_down_until_ms[0]>0);
 assert(recv(w[0].sv[0],&x,1,MSG_DONTWAIT)==0&&recv(w[1].sv[0],&x,1,MSG_DONTWAIT)==0);
 finish(&w[0],0);
 finish(&w[1],1);
 fprintf(stderr,"END\n");
 puts("client waiter cut: silent member cut, slow live member not, one exchange for two waiters PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-node-waiter-cut-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 r=subprocess.run([str(p.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=90)
 print(r.stdout,end='');err=r.stderr
 assert r.returncode==0,(r.returncode,err)
cases={c.split('\n')[0].strip():c for c in err.split('CASE ')[1:]}
cut={k:[l for l in v.splitlines() if l.startswith('node-waiter-cut:')] for k,v in cases.items()}
assert len(cut['1'])==1 and 'node=3 conns=1 ' in cut['1'][0],cut
assert cut['2']==[],cut                                        # a slow live member's request is never cut
assert len(cut['3'])==1 and 'node=3 conns=2 ' in cut['3'][0],cut
