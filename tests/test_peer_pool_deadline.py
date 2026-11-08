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
#include <stdarg.h>
void efs_telemetry_log(int critical, const char *format, ...) {
    (void)critical; va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
}
static int remaining=-1,waits,connects,destroys,live_fd=-1,rdma_on,upgrade_rc=-1;
static struct efs_conn live_conn;
static pthread_cond_t *last_signal;
static int watch_signal(pthread_cond_t *cv){last_signal=cv;return 0;}
int efs_net_remaining_ms(int cap){return remaining<0?cap:remaining<cap?remaining:cap;}
static uint64_t dl,connect_dl;
uint64_t efs_net_deadline_ms(void){return dl;}
void efs_net_set_deadline_ms(uint64_t d){dl=d;}
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
int efs_connect_tcp(const char *host,uint16_t port){(void)host;(void)port;connects++;connect_dl=dl;return live_fd;}
int efs_set_recv_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
int efs_set_send_timeout(int fd,int ms){(void)fd;(void)ms;return 0;}
struct efs_conn *efs_conn_wrap_tcp(int fd,int server){(void)server;return fd<0?NULL:&live_conn;}
void efs_conn_destroy(struct efs_conn *conn){(void)conn;destroys++;}
int efs_conn_peer_closed(const struct efs_conn *conn){(void)conn;return 0;}
int efs_rdma_available(void){return rdma_on;}
int efs_rdma_client_upgrade(struct efs_conn *conn){(void)conn;return upgrade_rc;}
int efs_conn_declare(struct efs_conn *c){(void)c;return 0;}
int efs_rdma_transport(void){return EFS_TRANSPORT_AUTO;}
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
 assert(last_signal==&p->cv[PEER_DATA]);p->busy[0]=1;
 p->busy[EFS_PEER_DATA_CONNS]=0;
 struct efs_conn *read=server_peer_read_conn_get("127.0.0.1",17432);
 assert(read==&owners[EFS_PEER_DATA_CONNS] && p->busy[EFS_PEER_DATA_CONNS]);
 server_peer_conn_release("127.0.0.1",17432,read);
 assert(last_signal==&p->cv[PEER_READ] && !p->busy[EFS_PEER_DATA_CONNS]);
 int before=waits;assert(!server_peer_conn_get("127.0.0.1",17432) && waits==before+1);
 read=server_peer_read_conn_get("127.0.0.1",17432);assert(read==&owners[EFS_PEER_DATA_CONNS]);
 p->busy[EFS_PEER_DATA_CONNS+1]=0;
 assert(server_peer_read_conn_get("127.0.0.1",17432)==&owners[EFS_PEER_DATA_CONNS+1]);
 before=waits;assert(!server_peer_read_conn_get("127.0.0.1",17432) && waits==before+1);
 remaining=0;assert(!server_peer_read_conn_get("127.0.0.1",17432) && waits==before+1);
 remaining=12;server_peer_conn_drop("127.0.0.1",17432,read);
 assert(destroys==1 && !p->conn[EFS_PEER_DATA_CONNS] && !p->busy[EFS_PEER_DATA_CONNS] && last_signal==&p->cv[PEER_READ]);
 assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==1 && !p->busy[EFS_PEER_DATA_CONNS]);
 for(int i=0;i<EFS_PEER_DATA_CONNS;i++)assert(p->busy[i] && p->conn[i]==&owners[i]);
 /* DQ: forwarded STATUS has its own slots. With every STATUS and data slot
  * held (each STATUS waiting on a leader that needs a session proof), the
  * leaf proof still checks out; STATUS never takes a proof slot. */
 int st=EFS_PEER_DATA_CONNS+EFS_PEER_READ_CONNS;
 assert(EFS_PEER_CONNS_PER_NODE==st+EFS_PEER_STATUS_CONNS);
 p->conn[EFS_PEER_DATA_CONNS]=&owners[EFS_PEER_DATA_CONNS];p->busy[EFS_PEER_DATA_CONNS+1]=0;
 for(int i=0;i<EFS_PEER_STATUS_CONNS;i++){p->busy[st+i]=0;
  assert(server_peer_status_conn_get("127.0.0.1",17432)==&owners[st+i]);}
 before=waits;assert(!server_peer_status_conn_get("127.0.0.1",17432) && waits==before+1);
 assert(!p->busy[EFS_PEER_DATA_CONNS] && !p->busy[EFS_PEER_DATA_CONNS+1]);
 struct efs_conn *proof=server_peer_read_conn_get("127.0.0.1",17432);
 assert(proof==&owners[EFS_PEER_DATA_CONNS]);
 server_peer_conn_release("127.0.0.1",17432,&owners[st]);
 assert(last_signal==&p->cv[PEER_STATUS] && !p->busy[st] && p->busy[EFS_PEER_DATA_CONNS]);
 server_peer_conn_release("127.0.0.1",17432,proof);assert(last_signal==&p->cv[PEER_READ]);
 p->busy[EFS_PEER_DATA_CONNS]=p->busy[EFS_PEER_DATA_CONNS+1]=1;
 before=waits;assert(server_peer_status_conn_get("127.0.0.1",17432)==&owners[st] && waits==before);
 before=waits;assert(!server_peer_read_conn_get("127.0.0.1",17432) && waits==before+1);
 p->busy[EFS_PEER_DATA_CONNS]=0;p->conn[EFS_PEER_DATA_CONNS]=NULL;
 /* A peer in its connect cooldown is refused without a connect until the
  * heartbeat reaches it; server_peer_note_up then ends the cooldown. */
 p->fail_streak=EFS_PEER_DOWN_FAILS;p->down_until_ms=peer_now_ms()+EFS_PEER_DOWN_MS;
 before=connects;assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before);
 server_peer_note_up("127.0.0.1",17433);assert(p->down_until_ms);  /* other peer, no slot made */
 for(int i=0;i<EFS_MAX_NODES;i++)assert(!(g_peers[i].in_use && g_peers[i].port==17433));
 server_peer_note_up("127.0.0.1",17432);assert(!p->down_until_ms && !p->fail_streak);
 assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before+1);
 /* K94: a pooled setup runs on the connect bound, not on what is left of
  * the caller's budget, and the caller's deadline is restored after it. */
 p->fail_streak=0;p->down_until_ms=0;dl=7;before=connects;assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before+1);
 assert(connect_dl+100>=peer_now_ms()+EFS_CONNECT_TIMEOUT_SEC*1000 && dl==7);
 dl=0;assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before+2 && !connect_dl);
 dl=peer_now_ms()+10000;uint64_t longer=dl;  /* a longer caller budget is never shortened */
 assert(!server_peer_read_conn_get("127.0.0.1",17432) && connects==before+3 && connect_dl==longer && dl==longer);dl=0;
 /* K100: in auto mode only an upgrade that failed before its request went
  * out (or that the peer refused) stays TCP; one that broke after the send
  * may have left the peer on RDMA, so the connection is destroyed. */
 live_fd=3;rdma_on=1;upgrade_rc=-1;before=destroys;
 assert(server_peer_conn_new("127.0.0.1",17432)==&live_conn && destroys==before);
 upgrade_rc=EFS_RDMA_UPGRADE_BROKEN;
 assert(!server_peer_conn_new("127.0.0.1",17432) && destroys==before+1);
 puts("peer pool: auto-mode upgrade broken after its send destroys the connection, an unsent or refused one stays tcp (K100) PASS");
 puts("peer pool: heartbeat ends a connect cooldown without creating slots PASS");
 puts("peer pool: pooled setup on at least the connect bound, a longer caller budget kept, caller deadline restored (K94) PASS");
 puts("peer pool: scoped wait12ms, unchanged unscoped5s, expired refusal, retained slot ownership and live checkout/release, saturated mutation pool, two isolated proof slots, STATUS slots apart from proofs (DQ) and class wakes PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-peer-deadline-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/server'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
