#!/usr/bin/env python3
"""The lease edges open.h does not expose: a failed row allocation hands off no
CREATE token, a competing OPEN waits at the stripe while an adoption reply is
withheld, an adoption retried after refused admission releases the stripe while
it waits, and a completed BUSY adoption keeps the row. The rest of this test's
edges are cases of tests/test_open_interface.c (Task ER)."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('efs_fuse')
def function(name):
 m=re.search(r'^(?:static )?(?:int|void|uint64_t) '+name+r'\([^;]+?\)\n\{',s,re.M)
 assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
a=s.index('struct efs_open_ref {')
b=s.index('/* Returns 1 when this is the first open',a)
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
static uint64_t stats_now_ms(void);
#include "client_internal.h"
static __thread uint64_t t_open_read_size;
uint64_t efs_client_map_clock(void){return 1;}
void efs_client_map_install(efs_ino_t i,const struct efs_chunk_rec *r,uint32_t n,uint64_t u){(void)i;(void)r;(void)n;(void)u;}
void efs_client_prefetch_open(efs_ino_t i,uint64_t s){(void)i;(void)s;}
#include "efs/session.h"
struct efs_client g_client;
struct fuse_file_info {int flags;};
static int efs_fuse_release_ino(efs_ino_t ino,struct fuse_file_info *fi);
static pthread_mutex_t adoption_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t adoption_cv=PTHREAD_COND_INITIALIZER;
static int pause_adoption,adoption_entered,adoption_release,opener_blocked;
static _Thread_local int following_opener;
static int open_rc,close_rc,adopt_rc,opens,closes,adopts,server_hold,revocation,adopt_begin_busy,adopt_resume_busy,adopt_known_busy,pressure_sleep,adopt_begins,combined_known_stale,combined_stale_unknown,combined_clear_begin_busy;
int efs_client_rpc_getchunks_fileid(efs_export_id_t ex,efs_ino_t ino,uint64_t gen,uint32_t start,struct efs_chunk_rec *recs,uint32_t *count,uint64_t *out) {
 (void)ex;(void)gen;(void)start;(void)recs;(void)count;assert(ino==42);*out=99;return EFS_OK;
}
int efs_client_lease_begin(struct efs_client_lease_request *r,efs_ino_t ino,uint64_t gen,unsigned flags,uint64_t token) {
 assert(!r->active&&ino==42&&gen==99);
 if(flags==EFS_LEASE_EDGE_CLEAR_PENDING&&combined_clear_begin_busy){combined_clear_begin_busy--;return EFS_ERR_BUSY;}
 assert(((flags&EFS_LEASE_EDGE_CLEAR_PENDING)&&token==3)||(!token&&flags!=EFS_LEASE_EDGE_CLEAR_PENDING));
 if(flags&EFS_LEASE_EDGE_CLEAR_PENDING){adopt_begins++;if(adopt_begin_busy){adopt_begin_busy--;pressure_sleep=1;return EFS_ERR_BUSY;}}
 *r=(struct efs_client_lease_request){.ino=ino,.generation=gen,.pending_token=token,.flags=flags,.active=1};r->q.id.seq=combined_known_stale && flags==EFS_LEASE_EDGE_CLEAR_PENDING ? 56 : 55;if(combined_stale_unknown && (flags&EFS_LEASE_EDGE_OPEN))r->generation=100;return EFS_OK;
}
int efs_client_lease_resume(struct efs_client_lease_request *r,int probe) {
 assert(!probe&&r->active);
 assert(!pthread_mutex_trylock(&g_open_mu));pthread_mutex_unlock(&g_open_mu);
 if(r->complete)return r->result;
 if(pause_adoption && (r->flags&EFS_LEASE_EDGE_CLEAR_PENDING) && (r->flags&EFS_LEASE_EDGE_OPEN)) {
  pthread_mutex_lock(&adoption_gate);adoption_entered=1;pthread_cond_broadcast(&adoption_cv);
  while(!adoption_release)pthread_cond_wait(&adoption_cv,&adoption_gate);
  pthread_mutex_unlock(&adoption_gate);
 }
 int rc;
 if(r->flags&EFS_LEASE_EDGE_OPEN){if(!r->attempted)opens++;
  if((r->flags&EFS_LEASE_EDGE_CLEAR_PENDING)&&combined_known_stale){adopts++;server_hold=0;if(!r->attempted&&combined_stale_unknown){r->attempted=1;return EFS_ERR_NET;}r->attempted=1;r->complete=1;r->result=EFS_ERR_STALE;return EFS_ERR_STALE;}
  server_hold=1;rc=open_rc;if(r->flags&EFS_LEASE_EDGE_CLEAR_PENDING){adopts++;assert(r->pending_token==3);}}
 else if(r->flags==EFS_LEASE_EDGE_CLEAR_PENDING){adopts++;assert((r->q.id.seq==55 || (combined_known_stale&&r->q.id.seq==56)) && r->pending_token==3);rc=adopt_rc;
  if(adopt_resume_busy){adopt_resume_busy--;pressure_sleep=1;rc=EFS_ERR_BUSY;}
  if(adopt_known_busy){r->complete=1;r->result=EFS_ERR_BUSY;return EFS_ERR_BUSY;}}
 else {closes++;rc=r->ino==43?EFS_OK:close_rc;if(!rc&&r->ino==42)server_hold=0;}
 r->attempted=1;
 if(revocation && rc==EFS_ERR_STALE){r->revoked=1;r->complete=1;r->result=rc;server_hold=0;}
 if(!rc){r->complete=1;r->result=rc;}return rc;
}
int efs_client_lease_reset(struct efs_client_lease_request *r) {
 assert(!r->active||r->complete);memset(r,0,sizeof(*r));return EFS_OK;
}
'''
# RPC mock references the production mutex, so place bookkeeping first.
mock=source[source.index('int efs_client_rpc_getchunks_fileid'):]
source=source[:source.index('int efs_client_rpc_getchunks_fileid')]+csrc.hblock('efs_fuse','#define CLOSE_ADMIT_SPINS')+s[a:b]+mock+r'''
static int fixture_sleep(unsigned us){
 if(following_opener) {
  pthread_mutex_lock(&adoption_gate);opener_blocked=1;pthread_cond_broadcast(&adoption_cv);pthread_mutex_unlock(&adoption_gate);
 }
 if(pressure_sleep){assert(!pthread_mutex_trylock(open_edge_mu(42)));pthread_mutex_unlock(open_edge_mu(42));pressure_sleep=0;}
 return usleep(us);
}
#define usleep fixture_sleep
void efs_client_opid_wait(unsigned max_ms){usleep(max_ms*1000u);}
static unsigned fail_open_alloc;
static void *fixture_calloc(size_t n,size_t size) {
 if(fail_open_alloc){fail_open_alloc=0;return NULL;}
 return calloc(n,size);
}
#define calloc fixture_calloc
'''
for name in ('wb_lock_budget','efs_open_note','efs_open_set_leased','efs_open_retry_order','efs_close_note','efs_open_release_pending_run','efs_open_release_pending','efs_open_drain_pending','efs_open_lease_resume_wait','efs_open_acquire_fileid_run','efs_open_acquire_fileid_mode','efs_open_acquire_fileid','efs_open_acquire','efs_open_adopt','efs_open_acquire_created'):
 source+='\n'+function(name)
w=s.index('/* A lost last-close reply must not pin')
x=s.index('/* Client-cache design Part A, pin rule 2:',w)
source+=r'''
static _Thread_local uint64_t mock_deadline;
uint64_t efs_client_rpc_deadline_ms(void){return mock_deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t deadline){mock_deadline=deadline;}
static int efs_pending_create_drain(void){return EFS_OK;}
static uint64_t stats_now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int efs_client_rpc_past_deadline(void){return mock_deadline && stats_now_ms()>=mock_deadline;}
'''+s[w:x]
source+=r'''
static int efs_fuse_release_ino(efs_ino_t ino,struct fuse_file_info *fi){
 (void)fi;int leased=0;if(efs_close_note(ino,&leased))return efs_open_release_pending(ino);return EFS_OK;
}
static void *racing_create(void *arg) {
 (void)arg;int handed=0;
 assert(!efs_open_acquire_created(42,99,3,&handed)&&handed);
 return NULL;
}
static void *racing_open(void *arg) {
 (void)arg;following_opener=1;
 assert(!efs_open_acquire(42));return NULL;
}
static void adoption_open_race(void) {
 pthread_t create_tid,open_tid;pause_adoption=1;
 assert(!pthread_create(&create_tid,NULL,racing_create,NULL));
 pthread_mutex_lock(&adoption_gate);
 while(!adoption_entered)pthread_cond_wait(&adoption_cv,&adoption_gate);
 pthread_mutex_unlock(&adoption_gate);
 assert(!pthread_create(&open_tid,NULL,racing_open,NULL));
 pthread_mutex_lock(&adoption_gate);
 while(!opener_blocked)pthread_cond_wait(&adoption_cv,&adoption_gate);
 /* The competing opener reached the actual stripe wait while adoption's
  * durable reply is withheld. No second lease/descriptor may pass it. */
 assert(g_open_refs && g_open_refs->n==1 && !server_hold && !opens);
 adoption_release=1;pthread_cond_broadcast(&adoption_cv);pthread_mutex_unlock(&adoption_gate);
 assert(!pthread_join(create_tid,NULL)&&!pthread_join(open_tid,NULL));
 assert(g_open_refs->n==2 && server_hold && opens==1 && adopts==1);
 pause_adoption=0;int leased=0;
 assert(!efs_close_note(42,&leased) && server_hold && !closes);
 assert(efs_close_note(42,&leased) && leased);
 assert(!efs_open_release_pending(42) && !g_open_refs && !server_hold && closes==1);
 opens=closes=adopts=0;
 puts("CREATE adoption/competing OPEN barrier: one lease, two descriptors, last-close ownership PASS");
}
int main(void) {
 /* Failed local open-owner allocation must transfer no pending CREATE token
  * and allocate no OPEN/adoption edge; the caller retains exact cleanup. */
 int handed_alloc=1;fail_open_alloc=1;
 assert(efs_open_acquire_created(42,99,3,&handed_alloc)==EFS_ERR_NOMEM);
 assert(!handed_alloc && !g_open_refs && !server_hold && !opens && !adopts);
 assert(!mock_deadline && !fail_open_alloc);
 adoption_open_race();
 int leased=0,handed_off=0;
 revocation=0;close_rc=0;adopt_rc=0;assert(!efs_open_acquire(42));
 adopt_begin_busy=3;adopt_resume_busy=2;int before_adopts=adopt_begins;
 assert(!efs_open_adopt(42,99,3,&handed_off) && handed_off && !g_open_refs->adoption.active);
 assert(adopt_begins==before_adopts+4 && !pressure_sleep && !adopt_begin_busy && !adopt_resume_busy);
 assert(efs_close_note(42,&leased));assert(!efs_open_release_pending(42));
 assert(!efs_open_acquire(42));adopt_known_busy=1;before_adopts=adopt_begins;
 assert(efs_open_adopt(42,99,3,&handed_off)==EFS_ERR_BUSY && handed_off && g_open_refs->adoption.complete);
 assert(adopt_begins==before_adopts+1);adopt_known_busy=0;
 assert(efs_close_note(42,&leased));
 assert(efs_open_release_pending(42)==EFS_ERR_BUSY && g_open_refs);
 g_open_refs->adoption.result=EFS_OK;assert(!efs_open_release_pending(42) && !g_open_refs);
 puts("FUSE lease retention: allocation failure, adoption barrier, adoption admission wait, completed BUSY adoption PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-retention-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
