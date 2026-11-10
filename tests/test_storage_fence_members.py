#!/usr/bin/env python3
"""Execute the storage-revocation counting rule (K54, K109): sealed, unreached, did-not-seal and refusing members, 3 to 5, with slow and changing rosters."""
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
static int gate,recorded; /* a row with a refusal: the sealing members answer after it is recorded */
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
 pthread_mutex_lock(&mu);spawned++;pthread_mutex_unlock(&mu);
 int rc=pthread_create(tid,NULL,tramp_run,t);
 if(rc){pthread_mutex_lock(&mu);spawned--;pthread_mutex_unlock(&mu);free(t);}
 return rc;
}
struct efs_raft_host {struct efsd_server *s;};
/* What each member answers: a reply rc, or one of these. */
enum {DOWN=1000,SLOW,BAD_UUID,BAD_FLOOR};
static int beh[EFS_MAX_NODES+1],mode,calls,done,roster_shrinks,released;
static struct efsd_server server;
static int answer(unsigned id,const struct efs_msg_storage_session_fence *q,struct efs_msg_storage_session_fence_reply *r) {
 assert(q->uuid[0]==17&&q->floor==2&&q->check_only==(unsigned)mode);
 __atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
 int b=beh[id];
 if(gate && b==0 && id!=server.id){pthread_mutex_lock(&mu);while(!recorded)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);}
 if(b==SLOW){pthread_mutex_lock(&mu);while(!released)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);b=0;}
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
 int down=answer(server.id,q,r);assert(!down);(void)down; /* this node always answers */
}
static int host_inode_rpc_addr(const char *host,uint16_t port,uint8_t type,const void *body,uint32_t bytes,uint8_t expected,void *out,uint32_t out_bytes) {
 assert(!strcmp(host,"127.0.0.1")&&port>9000&&port<=9000+EFS_MAX_NODES&&type==EFS_MSG_STORAGE_SESSION_FENCE&&expected==EFS_MSG_STORAGE_SESSION_FENCE_REPLY);
 assert(bytes==sizeof(struct efs_msg_storage_session_fence)&&out_bytes==sizeof(struct efs_msg_storage_session_fence_reply));
 return answer(port-9000,body,out);
}
'''+s[a:b]+r'''
static void reset(unsigned n) {
 pthread_mutex_lock(&server.lock);server.node_count=n;pthread_mutex_unlock(&server.lock);
 for(unsigned i=0;i<n;i++){server.nodes[i].id=i+1;snprintf(server.nodes[i].addr,sizeof(server.nodes[i].addr),"127.0.0.1");server.nodes[i].port=9001+i;}
 pthread_mutex_lock(&mu);
 memset(beh,0,sizeof(beh));calls=done=roster_shrinks=released=gate=recorded=spawned=exited=0;
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
int main(void) {
 pthread_mutex_init(&server.lock,NULL);server.id=1;
 struct efs_raft_host host={&server};uint8_t uuid[16]={17};
 for(mode=0;mode<2;mode++)
  for(unsigned r=0;r<sizeof(rows)/sizeof(rows[0]);r++){
   reset(rows[r].n);memcpy(beh+1,rows[r].a,rows[r].n*sizeof(int));
   for(unsigned i=1;i<rows[r].n;i++)gate|=rows[r].a[i]==ST||rows[r].a[i]==IV||rows[r].a[i]==BAD_UUID||rows[r].a[i]==BAD_FLOOR;
   int rc=host_storage_fence_members(&host,uuid,2,mode);
   if(rc!=rows[r].rc){fprintf(stderr,"row %u mode %d: got %d want %d\n",r,mode,rc,rows[r].rc);abort();}
   settle((int)rows[r].n);
  }
 /* A slow member is not waited for once a majority sealed; its answer is left to arrive. */
 static const struct {unsigned n;int a[5];} slow[]={{3,{O,O,S}},{3,{O,S,O}},{5,{O,O,O,S,S}},{5,{O,S,O,O,S}},{4,{O,O,O,S}}};
 for(mode=0;mode<2;mode++)
  for(unsigned r=0;r<sizeof(slow)/sizeof(slow[0]);r++){
   reset(slow[r].n);memcpy(beh+1,slow[r].a,slow[r].n*sizeof(int));
   assert(host_storage_fence_members(&host,uuid,2,mode)==0);
   settle((int)slow[r].n);assert(calls==(int)slow[r].n);
  }
 mode=0;
 /* A slow member that decides the majority is waited for, and a refusal does not wait for a slow one. */
 reset(3);beh[2]=ST;beh[3]=S;gate=0;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_STALE);settle(3);
 /* The roster changing under the call, and a duplicate or short roster. */
 reset(4);roster_shrinks=1;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_BUSY);settle(4);
 reset(4);server.nodes[3].id=3;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_INVAL);
 reset(4);server.node_count=2;assert(host_storage_fence_members(&host,uuid,2,0)==EFS_ERR_BUSY);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-storage-members-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread']+cflags()+['-I'+str(root),'-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run(wrap()+[str(p.with_suffix(''))],check=True)
print('storage revocation: strict majority of members sealed; unreached or not-sealed answers count alike, a slow member is not awaited, refusals (STALE, INVAL, PROTO) seen before a majority sealed fail it, changed roster and duplicate identity rejection PASS')
