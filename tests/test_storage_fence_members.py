#!/usr/bin/env python3
"""Execute the storage-revocation counting rule (K54, K109): sealed, unreached, did-not-seal and refusing members, 3 to 5, with slow, late-refusing and changing rosters, with a thread per member and with thread creation failing; the per-member cap on waiting exchanges (a member that never answers holds at most HOST_FENCE_MAX_INFLIGHT workers, concurrent healthy rounds are not refused); and the worker registry the host stop uses (arm, disarm, wake, drain, resume) against real sockets."""
from pathlib import Path
import os,shlex,subprocess,tempfile
import csrc
from sanitize_env import cflags,wrap
root=Path(__file__).resolve().parents[1]
s=csrc.module('raft_host')
a=s.index('enum {FENCE_PENDING');b=s.index('\n}',s.index('static int host_storage_fence_members('))+2
code=r'''
#include "src/server/server_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>
static int gate,recorded,fail_spawn; /* a row with a refusal: the sealing members answer after it is recorded */
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
void efs_telemetry_log(int critical, const char *format, ...) {
    (void)critical; va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
    if(strstr(format,"rc=%d")){pthread_mutex_lock(&mu);recorded=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);}
}
/* Workers are counted in and out so that a row ends only when every worker, its logging included, has finished. */
static int spawned,exited;
struct tramp {void *(*fn)(void *);void *arg;};
static void *tramp_run(void *p) {
 struct tramp t=*(struct tramp *)p;free(p);t.fn(t.arg);
 pthread_mutex_lock(&mu);exited++;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 return NULL;
}
int efsd_pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg) {
 struct tramp *t=malloc(sizeof(*t));t->fn=fn;t->arg=arg;
 if(fail_spawn){free(t);return EAGAIN;}
 pthread_mutex_lock(&mu);spawned++;pthread_mutex_unlock(&mu);
 int rc=pthread_create(tid,NULL,tramp_run,t);
 if(rc){pthread_mutex_lock(&mu);spawned--;pthread_mutex_unlock(&mu);free(t);}
 return rc;
}
struct efs_raft_host {struct efsd_server *s;};
/* What each member answers: a reply rc, or one of these. */
enum {DOWN=1000,SLOW,BAD_UUID,BAD_FLOOR,LATE_ST,LATE_IV,LATE_PR}; /* LATE_*: the refusal comes only after the call returned */
static int beh[EFS_MAX_NODES+1],mode,calls,local_calls,done,roster_shrinks,released;
static struct efsd_server server;
static int answer(unsigned id,const struct efs_msg_storage_session_fence *q,struct efs_msg_storage_session_fence_reply *r) {
 assert(q->uuid[0]==17&&q->floor==2&&q->check_only==(unsigned)mode);
 __atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
 int b=beh[id];
 if(gate && b==0 && id!=server.id){pthread_mutex_lock(&mu);while(!recorded)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);}
 if(b==SLOW||b>=LATE_ST){
  pthread_mutex_lock(&mu);while(!released)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);
  b=b==SLOW?0:b==LATE_ST?EFS_ERR_STALE:b==LATE_IV?EFS_ERR_INVAL:BAD_UUID;
 }
 if(b!=DOWN){
  memcpy(r->uuid,q->uuid,16);r->floor=q->floor;r->rc=b==BAD_UUID||b==BAD_FLOOR?0:b;
  if(b==BAD_UUID)r->uuid[0]^=1;
  if(b==BAD_FLOOR)r->floor++;
 }
 pthread_mutex_lock(&mu);done++;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 return b==DOWN?-1:0;
}
void server_raft_host_storage_session_fence(const struct efs_msg_storage_session_fence *q,struct efs_msg_storage_session_fence_reply *r) {
 if(roster_shrinks){pthread_mutex_lock(&server.lock);server.node_count=3;pthread_mutex_unlock(&server.lock);}
__atomic_fetch_add(&local_calls,1,__ATOMIC_RELAXED);
 int down=answer(server.id,q,r);assert(!down);(void)down; /* this node always answers */
}
static int host_fence_rpc(const char *host,uint16_t port,const void *body,uint32_t bytes,void *out,uint32_t out_bytes) {
 assert(!strcmp(host,"127.0.0.1")&&port>9000&&port<=9000+EFS_MAX_NODES);
 assert(bytes==sizeof(struct efs_msg_storage_session_fence)&&out_bytes==sizeof(struct efs_msg_storage_session_fence_reply));
 return answer(port-9000,body,out);
}
'''+s[a:b]+r'''
static void reset(unsigned n) {
 pthread_mutex_lock(&server.lock);server.node_count=n;pthread_mutex_unlock(&server.lock);
 for(unsigned i=0;i<n;i++){server.nodes[i].id=i+1;snprintf(server.nodes[i].addr,sizeof(server.nodes[i].addr),"127.0.0.1");server.nodes[i].port=9001+i;}
 pthread_mutex_lock(&mu);
 memset(beh,0,sizeof(beh));calls=local_calls=done=roster_shrinks=released=gate=recorded=spawned=exited=0;
 pthread_mutex_unlock(&mu);
}
/* Every member that was asked has answered; the late ones are released first. */
static void settle(int want) {
 pthread_mutex_lock(&mu);released=1;pthread_cond_broadcast(&cv);
 while(done<want || exited<spawned)pthread_cond_wait(&cv,&mu);
 pthread_mutex_unlock(&mu);
}
/* The counting rule: rows are (members, answers of members 1..n, expected result). Member 1 is this node. */
#define O 0
#define D DOWN
#define S SLOW
#define NP EFS_ERR_NOT_PRIMARY
#define BU EFS_ERR_BUSY
#define IO EFS_ERR_IO
#define NF EFS_ERR_NOT_FOUND
#define ST EFS_ERR_STALE
#define IV EFS_ERR_INVAL
#define PR EFS_ERR_PROTO
static const struct {unsigned n;int a[5];int rc;} rows[]={
 /* sealed, unreached */
 {3,{O,O,O},0},{3,{O,O,D},0},{3,{O,D,D},EFS_ERR_BUSY},
 {4,{O,O,D,O},0},{4,{O,O,D,D},EFS_ERR_BUSY},
 {5,{O,O,O,D,D},0},{5,{O,O,D,D,D},EFS_ERR_BUSY},{5,{O,D,D,D,D},EFS_ERR_BUSY},
 /* answered that it did not seal: counts like an unreached member (K109) */
 {3,{O,O,NP},0},{3,{O,NP,O},0},{3,{NP,O,O},0},{3,{O,O,BU},0},{3,{O,O,IO},0},{3,{O,O,NF},0},{3,{O,NP,D},NP},
 {3,{O,NP,NP},NP},{3,{O,BU,NP},BU},{3,{NP,NP,NP},NP},{3,{O,D,IO},IO},
 {5,{O,O,O,NP,NP},0},{5,{O,NP,O,NP,O},0},{5,{O,O,O,IO,D},0},{5,{O,O,NP,NP,D},NP},{5,{O,O,D,BU,NP},BU},
 /* a refusal answered before the majority sealed fails the call (the rows hold the sealers back until it is recorded) */
 {3,{O,O,ST},ST},{3,{ST,O,O},ST},{3,{O,ST,O},ST},{3,{O,O,IV},IV},{3,{O,O,BAD_UUID},PR},{3,{O,BAD_FLOOR,O},PR},
 {5,{O,O,O,O,ST},ST},{5,{O,O,O,D,IV},IV},{5,{O,O,O,O,BAD_UUID},PR},
};
static int hammer_failures;
static void *hammer(void *arg) {
 uint8_t uuid[16]={17};
 for(int i=0;i<200;i++)if(host_storage_fence_members(arg,uuid,2,0)!=0)__atomic_fetch_add(&hammer_failures,1,__ATOMIC_RELAXED);
 return NULL;
}
int main(void) {
 pthread_mutex_init(&server.lock,NULL);server.id=1;
 struct efs_raft_host host={&server};uint8_t uuid[16]={17};
 /* Each row runs with a thread per remote member, then with thread creation failing (every member is asked inline).
  * Every member is asked exactly once and this node's check runs once, whatever the answers, so those counts are checked
  * after the late workers settle. At return the counts are exact only where the call had to wait for every member
  * (no majority reached, or all asked inline); a call that returns on a majority or a refusal leaves a varying number
  * of workers unanswered, so a row there has no exact count at return. */
 for(fail_spawn=0;fail_spawn<2;fail_spawn++)
 for(mode=0;mode<2;mode++)
  for(unsigned r=0;r<sizeof(rows)/sizeof(rows[0]);r++){
   reset(rows[r].n);memcpy(beh+1,rows[r].a,rows[r].n*sizeof(int));
   int refusal=0;
   for(unsigned i=0;i<rows[r].n;i++)refusal|=rows[r].a[i]==ST||rows[r].a[i]==IV||rows[r].a[i]==BAD_UUID||rows[r].a[i]==BAD_FLOOR;
   gate=refusal&&!fail_spawn;
   int rc=host_storage_fence_members(&host,uuid,2,mode);
   if(rc!=rows[r].rc){fprintf(stderr,"row %u mode %d inline %d: got %d want %d\n",r,mode,fail_spawn,rc,rows[r].rc);abort();}
   pthread_mutex_lock(&mu);int d=done,sp=spawned;pthread_mutex_unlock(&mu);
   assert(local_calls==1); /* this node's check runs inline before the wait */
   if(fail_spawn){assert(sp==0&&d==(int)rows[r].n);}
   else if(rc!=0&&!refusal){assert(d==(int)rows[r].n);} /* no majority: the call waited for every member */
   settle((int)rows[r].n);
   assert(calls==(int)rows[r].n&&local_calls==1);
  }
 fail_spawn=0;
 /* A slow member is not waited for once a majority sealed; its answer is left to arrive. */
 static const struct {unsigned n;int a[5];} slow[]={{3,{O,O,S}},{3,{O,S,O}},{5,{O,O,O,S,S}},{5,{O,S,O,O,S}},{4,{O,O,O,S}}};
 for(mode=0;mode<2;mode++)
  for(unsigned r=0;r<sizeof(slow)/sizeof(slow[0]);r++){
   reset(slow[r].n);memcpy(beh+1,slow[r].a,slow[r].n*sizeof(int));
   assert(host_storage_fence_members(&host,uuid,2,mode)==0);
   assert(local_calls==1);
   settle((int)slow[r].n);assert(calls==(int)slow[r].n&&local_calls==1);
  }
 /* A refusal that arrives after the majority sealed is not seen: the call returned OK and the refusal is only logged.
  * (It cannot be, and is not, relied on: the epoch-bound FINISH decides, see the status page.) */
 static const struct {unsigned n;int a[5];} late[]={{3,{O,O,LATE_ST}},{3,{O,LATE_IV,O}},{3,{O,O,LATE_PR}},{5,{O,O,O,LATE_ST,LATE_PR}},{4,{O,O,O,LATE_IV}}};
 for(mode=0;mode<2;mode++)
  for(unsigned r=0;r<sizeof(late)/sizeof(late[0]);r++){
   reset(late[r].n);memcpy(beh+1,late[r].a,late[r].n*sizeof(int));
   assert(host_storage_fence_members(&host,uuid,2,mode)==0);
   pthread_mutex_lock(&mu);assert(!recorded&&done<(int)late[r].n);pthread_mutex_unlock(&mu); /* still unanswered at return */
   settle((int)late[r].n);
   assert(calls==(int)late[r].n&&recorded); /* the refusal arrived and was logged, after the call had returned OK */
  }
 mode=0;
 /* A member that never answers holds at most HOST_FENCE_MAX_INFLIGHT workers however many rounds arrive; each round still
  * returns the majority result at once (this node and member 2). Past the cap the member counts unreached without a worker. */
 reset(3);beh[3]=S;
 for(int round=0;round<100;round++)assert(host_storage_fence_members(&host,uuid,2,0)==0);
 pthread_mutex_lock(&mu);assert(spawned==100+HOST_FENCE_MAX_INFLIGHT);pthread_mutex_unlock(&mu); /* 100 workers for member 2, the cap for member 3 */
 assert(g_fence_slots[3]==HOST_FENCE_MAX_INFLIGHT);
 settle(200+HOST_FENCE_MAX_INFLIGHT);assert(calls==100+100+HOST_FENCE_MAX_INFLIGHT);
 for(unsigned id=0;id<=EFS_MAX_NODES;id++)assert(g_fence_slots[id]==0); /* every slot is given back */
 /* Every remote member at the cap: no worker is started and the round fails closed at once (only this node sealed). */
 reset(3);g_fence_slots[2]=g_fence_slots[3]=HOST_FENCE_MAX_INFLIGHT;
 assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_BUSY);
 pthread_mutex_lock(&mu);assert(spawned==0&&calls==1);pthread_mutex_unlock(&mu);
 g_fence_slots[2]=g_fence_slots[3]=0;
 /* One of two at the cap: the other member and this node are a majority. */
 reset(3);g_fence_slots[3]=HOST_FENCE_MAX_INFLIGHT;
 assert(host_storage_fence_members(&host,uuid,2,0)==0);
 settle(2);assert(calls==2);g_fence_slots[3]=0;
 /* Concurrent unmounts ask the same healthy members at once; none may be refused for sharing a member (a cap of one would). */
 reset(3);
 {
  pthread_t t[8];
  for(int i=0;i<8;i++)assert(!pthread_create(&t[i],NULL,hammer,&host));
  for(int i=0;i<8;i++)pthread_join(t[i],NULL);
  assert(hammer_failures==0);
  settle(0);
  for(unsigned id=0;id<=EFS_MAX_NODES;id++)assert(g_fence_slots[id]==0);
 }
 /* A slow member that decides the majority is waited for, and a refusal does not wait for a slow one. */
 reset(3);beh[2]=ST;beh[3]=S;gate=0;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_STALE);settle(3);
 /* The roster changing under the call, and a duplicate or short roster. */
 reset(4);roster_shrinks=1;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_BUSY);settle(4);
 reset(4);server.nodes[3].id=3;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_INVAL);
 reset(4);server.node_count=2;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_BUSY);
 return 0;
}
'''
# The worker registry the host stop uses: real sockets, stubbed connection setup.
ra=s.index('struct host_fence_conn{');rb=s.index('/* A metadata quorum is not a storage-member')
reg=r'''
#include "src/server/server_internal.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
enum {ANSWER,PARK_EXCHANGE,PARK_HANDSHAKE};
static int behave,connects,waiting,logged;
static char last[160];
void efs_telemetry_log(int critical,const char *format,...) {
 (void)critical;va_list ap;va_start(ap,format);pthread_mutex_lock(&mu);vsnprintf(last,sizeof(last),format,ap);logged++;pthread_mutex_unlock(&mu);va_end(ap);
}
/* A worker has reached the point it waits at (a read that only a shutdown of its socket ends). */
static void park(int fd) {
 pthread_mutex_lock(&mu);waiting++;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 char b;(void)!read(fd,&b,1);
}
static int peer_end[8];
struct efs_conn *server_peer_conn_new_armed(const char *host,uint16_t port,const struct server_peer_arm *ar) {
 (void)host;(void)port;int sv[2];
 pthread_mutex_lock(&mu);int n=connects++;pthread_mutex_unlock(&mu);
 assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sv));peer_end[n%8]=sv[1];
 if(ar->arm(sv[0],ar->ctx)){close(sv[0]);close(sv[1]);return NULL;} /* refused once the stop began */
 if(behave==PARK_HANDSHAKE){park(sv[0]);ar->disarm(ar->ctx);close(sv[0]);return NULL;} /* the declare handshake with a stalled member */
 ar->disarm(ar->ctx); /* as peer_connect: the caller lists the socket again for the exchange */
 struct efs_conn *c=calloc(1,sizeof(*c));c->fd=sv[0];return c;
}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n) {(void)c;(void)type;(void)p;(void)n;return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **payload,uint32_t *len) {
 if(behave==PARK_EXCHANGE){park(c->fd);return -1;} /* woken: the socket was shut down */
 struct efs_msg_storage_session_fence_reply *r=calloc(1,sizeof(*r));r->floor=7;
 *type=EFS_MSG_STORAGE_SESSION_FENCE_REPLY;*payload=r;*len=sizeof(*r);return 0;
}
void efs_conn_destroy(struct efs_conn *c) {close(c->fd);free(c);}
'''+s[ra:rb]+r'''
struct call {int rc;pthread_t t;};
static void *rpc(void *arg) {
 struct call *k=arg;struct efs_msg_storage_session_fence q={0};struct efs_msg_storage_session_fence_reply r={0};
 k->rc=host_fence_rpc("127.0.0.1",9001,&q,sizeof(q),&r,sizeof(r));
 if(k->rc==0)assert(r.floor==7);
 return NULL;
}
static void start(struct call *k,int how,int want_waiting) {
 pthread_mutex_lock(&mu);behave=how;pthread_mutex_unlock(&mu);
 assert(!pthread_create(&k->t,NULL,rpc,k));
 pthread_mutex_lock(&mu);while(waiting<want_waiting)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);
}
static void reset_log(void) {pthread_mutex_lock(&mu);logged=waiting=0;last[0]=0;pthread_mutex_unlock(&mu);}
int main(void) {
 struct call k[2];
 /* An exchange that completes is listed and unlisted again; nothing is left. */
 reset_log();start(&k[0],ANSWER,0);pthread_join(k[0].t,NULL);assert(k[0].rc==0&&!g_fence_conns&&!g_fence_active);
 /* Both stalled waits are woken by the drain: the receive of an exchange, and the handshake before it. */
 for(int how=PARK_EXCHANGE;how<=PARK_HANDSHAKE;how++){
  reset_log();start(&k[0],how,1);
  host_fence_drain();
  pthread_join(k[0].t,NULL);
  assert(k[0].rc==-1&&!g_fence_conns&&!g_fence_active);
  assert(logged==1&&!strcmp(last,"storage-fence: stop woke 1 worker(s) waiting on a member\n"));
  /* stopping: a new exchange is refused before it connects */
  int before=connects;assert(host_fence_rpc("127.0.0.1",9001,&(struct efs_msg_storage_session_fence){0},sizeof(struct efs_msg_storage_session_fence),&(struct efs_msg_storage_session_fence_reply){0},sizeof(struct efs_msg_storage_session_fence_reply))==-1);
  assert(connects==before);
  host_fence_resume();
  start(&k[0],ANSWER,0);pthread_join(k[0].t,NULL);assert(k[0].rc==0); /* asks again after a restart */
 }
 /* wake (the handler drain) before drain (the host stop): two workers are woken once, counted once, and wake does not wait. */
 reset_log();start(&k[0],PARK_EXCHANGE,1);start(&k[1],PARK_EXCHANGE,2);
 server_raft_host_fence_wake();
 pthread_join(k[0].t,NULL);pthread_join(k[1].t,NULL);assert(k[0].rc==-1&&k[1].rc==-1);
 assert(logged==0); /* only the drain logs */
 host_fence_drain();
 assert(logged==1&&!strcmp(last,"storage-fence: stop woke 2 worker(s) waiting on a member\n"));
 host_fence_resume();
 /* A socket opened after the stop began is refused by arm. */
 server_raft_host_fence_wake();
 assert(host_fence_rpc("127.0.0.1",9001,&(struct efs_msg_storage_session_fence){0},sizeof(struct efs_msg_storage_session_fence),&(struct efs_msg_storage_session_fence_reply){0},sizeof(struct efs_msg_storage_session_fence_reply))==-1);
 host_fence_resume();
 assert(!g_fence_conns&&!g_fence_active&&!g_fence_woken);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-storage-members-') as directory:
 for name,src in (('t',code),('r',reg)):
  p=Path(directory)/(name+'.c');p.write_text(src)
  subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread']+cflags()+['-I'+str(root),'-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
  subprocess.run(wrap()+[str(p.with_suffix(''))],check=True)
print('storage revocation: a member holds at most HOST_FENCE_MAX_INFLIGHT workers and concurrent rounds are not refused, the stop wakes and joins workers waiting in a handshake or an exchange (wake before drain counts once); strict majority of members sealed; unreached or not-sealed answers count alike, a slow member is not awaited, refusals (STALE, INVAL, PROTO) seen before a majority sealed fail it and later ones are only logged, every member asked once (threads and inline fallback), changed roster and duplicate identity rejection PASS')
