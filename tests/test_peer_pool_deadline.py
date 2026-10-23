#!/usr/bin/env python3
"""Production pool checkout honors the caller budget without borrowing slots."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "efs/network.h"
#include "efs/rdma.h"
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
static int remaining=-1,waits,connects,destroys;
static pthread_cond_t *last_signal;
static int watch_signal(pthread_cond_t *cv){last_signal=cv;return 0;}
int efs_net_remaining_ms(int cap){return remaining<0?cap:remaining<cap?remaining:cap;}
static int controlled_wait(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *end){
 (void)cv;(void)mu;struct timespec now;clock_gettime(CLOCK_REALTIME,&now);
 long long delta=(end->tv_sec-now.tv_sec)*1000000000LL+end->tv_nsec-now.tv_nsec;
 assert(delta>0 && delta<=(remaining<0?5000:remaining)*1000000LL);
 assert(delta>=(remaining<0?4990:2)*1000000LL);waits++;return ETIMEDOUT;}
#define pthread_cond_timedwait controlled_wait
#define pthread_cond_signal watch_signal
#include "peer_pool.c"
#undef pthread_cond_timedwait
#undef pthread_cond_signal
int efs_connect_tcp(const char *host,uint16_t port){(void)host;(void)port;connects++;return -1;}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)fd;(void)server;return NULL;}
void efs_conn_destroy(struct efs_conn *conn){(void)conn;destroys++;}
int efs_rdma_available(void){return 0;}
int efs_rdma_client_upgrade(struct efs_conn *conn){(void)conn;return -1;}
int efs_conn_declare_as(struct efs_conn *c,uint8_t role,const struct efs_identity *id,struct efs_msg_client_hello_reply *r){(void)c;(void)role;(void)id;(void)r;return 0;}
static struct efs_cluster_conf conf={.n=3};
const struct efs_cluster_conf *efs_cluster_conf_current(void){return &conf;}
uint32_t efs_cluster_conf_member_at(const struct efs_cluster_conf *c,const char *a,uint16_t p){(void)c;(void)a;(void)p;return 2;}
void adm_local_identity(struct efs_identity *o){memset(o,0,sizeof(*o));}
int adm_check_reply(uint32_t m,const struct efs_msg_client_hello_reply *r,char *w,size_t n){(void)m;(void)r;(void)w;(void)n;return 0;}
uint64_t adm_generation(void){return 0;}
int efs_rdma_transport(void){return EFS_TRANSPORT_TCP;}
int main(void){struct efs_conn owners[EFS_PEER_CONNS_PER_NODE];
 struct peer_slot *p=peer_slot_for("127.0.0.1",17432);assert(p);
 for(int i=0;i<EFS_PEER_CONNS_PER_NODE;i++){p->conn[i]=&owners[i];p->busy[i]=1;}
 remaining=12;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==1 && !connects);
 remaining=-1;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==2 && !connects);
 remaining=0;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==2);
 p->busy[0]=0;assert(!server_peer_conn_get("127.0.0.1",17432) && !p->busy[0]);
 remaining=12;assert(server_peer_conn_get("127.0.0.1",17432)==&owners[0] && p->busy[0]);
 server_peer_conn_release("127.0.0.1",17432,&owners[0]);assert(!p->busy[0]);
 for(int i=1;i<EFS_PEER_CONNS_PER_NODE;i++)assert(p->busy[i] && p->conn[i]==&owners[i]);
 /* All ordinary slots can remain owned while two isolated reads progress. */
 assert(last_signal==&p->cv);p->busy[0]=1;
 p->busy[EFS_PEER_DATA_CONNS]=0;
 struct efs_conn *read=server_peer_read_conn_get("127.0.0.1",17432);
 assert(read==&owners[EFS_PEER_DATA_CONNS] && p->busy[EFS_PEER_DATA_CONNS]);
 server_peer_conn_release("127.0.0.1",17432,read);
 assert(last_signal==&p->read_cv && !p->busy[EFS_PEER_DATA_CONNS]);
 int before=waits;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==before+1);
 read=server_peer_read_conn_get("127.0.0.1",17432);assert(read==&owners[EFS_PEER_DATA_CONNS]);
 p->busy[EFS_PEER_DATA_CONNS+1]=0;
 assert(server_peer_read_conn_get("127.0.0.1",17432)==&owners[EFS_PEER_DATA_CONNS+1]);
 before=waits;assert(!server_peer_read_conn_get("127.0.0.1",17432) && waits==before+1);
 remaining=0;assert(!server_peer_read_conn_get("127.0.0.1",17432) && waits==before+1);
 remaining=12;server_peer_conn_drop("127.0.0.1",17432,read);
 assert(destroys==1 && !p->conn[EFS_PEER_DATA_CONNS] && !p->busy[EFS_PEER_DATA_CONNS] && last_signal==&p->read_cv);
 assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==1 && !p->busy[EFS_PEER_DATA_CONNS]);
 for(int i=0;i<EFS_PEER_DATA_CONNS;i++)assert(p->busy[i] && p->conn[i]==&owners[i]);
 /* A peer in its connect cooldown is refused without a connect until the
  * heartbeat reaches it; server_peer_note_up then ends the cooldown. */
 p->fail_streak=EFS_PEER_DOWN_FAILS;p->down_until_ms=peer_now_ms()+EFS_PEER_DOWN_MS;
 before=connects;assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before);
 server_peer_note_up("127.0.0.1",17433);assert(p->down_until_ms);  /* other peer, no slot made */
 for(int i=0;i<EFS_MAX_NODES;i++)assert(!(g_peers[i].in_use && g_peers[i].port==17433));
 server_peer_note_up("127.0.0.1",17432);assert(!p->down_until_ms && !p->fail_streak);
 assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before+1);
 puts("peer pool: heartbeat ends a connect cooldown without creating slots PASS");
 puts("peer pool: scoped wait12ms, unchanged unscoped5s, expired refusal, retained slot ownership and live checkout/release, saturated mutation pool, two isolated proof slots and class wakes PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-peer-deadline-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/server'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
