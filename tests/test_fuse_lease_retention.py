#!/usr/bin/env python3
"""Execute production lease bookkeeping through ambiguous sequenced replies."""
from pathlib import Path
import os,re,resource,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
 m=re.search(r'^static (?:int|void|uint64_t) '+name+r'\([^;]+?\)\n\{',s,re.M)
 assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
a=s.index('static pthread_mutex_t g_open_mu')
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
source=source[:source.index('int efs_client_rpc_getchunks_fileid')]+s[a:b]+mock+r'''
static int fixture_sleep(unsigned us){
 if(following_opener) {
  pthread_mutex_lock(&adoption_gate);opener_blocked=1;pthread_cond_broadcast(&adoption_cv);pthread_mutex_unlock(&adoption_gate);
 }
 if(pressure_sleep){assert(!pthread_mutex_trylock(open_edge_mu(42)));pthread_mutex_unlock(open_edge_mu(42));pressure_sleep=0;}
 return usleep(us);
}
#define usleep fixture_sleep
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
 assert(!efs_open_acquire(42));assert(!efs_open_acquire(42));assert(opens==1);
 int leased=0;assert(!efs_close_note(42,&leased));assert(!leased);
 assert(efs_close_note(42,&leased)&&leased);
 close_rc=EFS_ERR_NET;assert(efs_open_release_pending(42)==EFS_ERR_NET);
 assert(g_open_refs&&!g_open_refs->n&&g_open_refs->close_pending&&server_hold);
 assert(efs_open_acquire(42)==EFS_ERR_NET&&opens==1);
 close_rc=0;assert(!efs_open_acquire(42));assert(opens==2&&g_open_refs->n==1&&!g_open_refs->close_pending);
 assert(efs_close_note(42,&leased));close_rc=EFS_ERR_NET;
 assert(efs_open_drain_pending()==EFS_ERR_NET&&g_open_refs);
 close_rc=0;assert(efs_open_drain_pending()==EFS_ERR_BUSY&&!g_open_refs&&!server_hold);
 assert(!efs_open_drain_pending());
 open_rc=EFS_ERR_NET;close_rc=EFS_ERR_NET;
 assert(efs_open_acquire(42)==EFS_ERR_NET&&server_hold&&g_open_refs&&!g_open_refs->n);
 int prior=opens;assert(efs_open_acquire(42)==EFS_ERR_NET&&opens==prior);
 open_rc=0;close_rc=0;assert(efs_open_drain_pending()==EFS_ERR_BUSY&&!g_open_refs&&!server_hold);
 open_rc=0;assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 assert(!efs_open_release_pending(42)&&!g_open_refs);
 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 close_rc=EFS_ERR_NOT_FOUND;assert(efs_open_release_pending(42)==EFS_ERR_NOT_FOUND&&g_open_refs&&server_hold);
 close_rc=0;assert(!efs_open_release_pending(42)&&!g_open_refs&&!server_hold);
 /* A CLOSE allocated after close_note must outrank a later CLOSE that
  * needs admission. This is the live one-inflight/window-full deadlock. */
 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 assert(g_open_refs->retry_order==UINT64_MAX);
 close_rc=EFS_ERR_NET;assert(efs_open_release_pending(42)==EFS_ERR_NET);
 assert(g_open_refs->edge.q.id.seq==55 && g_open_refs->retry_order==55);
 struct efs_open_ref *unallocated=calloc(1,sizeof(*unallocated));assert(unallocated);
 unallocated->ino=43;unallocated->close_pending=1;
 unallocated->retry_order=efs_open_retry_order(unallocated);
 unallocated->next=g_open_refs;g_open_refs=unallocated;
 close_rc=0;assert(efs_open_drain_pending()==EFS_ERR_BUSY);
 assert(g_open_refs==unallocated && !unallocated->next && !server_hold);
 g_open_refs=NULL;free(unallocated);
 /* Adoption's identity also owns admission even when edge is empty. */
 struct efs_open_ref adoption_only={0};adoption_only.adoption.active=1;
 adoption_only.adoption.q.id.seq=7;
 assert(efs_open_retry_order(&adoption_only)==7);
 adoption_only.adoption.complete=1;assert(efs_open_retry_order(&adoption_only)==UINT64_MAX);
 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 /* A retry cannot block forever on a foreground inode stripe. Keep the
  * exact owner and retry it after the deadline/stripe clears. */
 pthread_mutex_lock(open_edge_mu(42));
 mock_deadline=stats_now_ms()+15;
 assert(efs_open_drain_pending()==EFS_ERR_BUSY&&g_open_refs&&server_hold);
 assert(efs_client_rpc_past_deadline());
 pthread_mutex_unlock(open_edge_mu(42));mock_deadline=0;
 g_open_refs->retry_order=1;g_open_refs->retry_turn=0;
 struct efs_open_ref *newer=calloc(1,sizeof(*newer));assert(newer);
 newer->ino=43;newer->close_pending=1;newer->retry_order=99;newer->next=g_open_refs;g_open_refs=newer;
 assert(efs_open_drain_pending()==EFS_ERR_BUSY&&g_open_refs==newer&&!newer->next);
 g_open_refs=NULL;free(newer);
 /* One unrecoverable/unknown old close cannot starve another retained
  * identity until the mount's shared ACK window fills. Neither is dropped. */
 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 close_rc=EFS_ERR_NET;
 /* Both rows must own allocated identities to exercise turn fairness. */
 g_open_refs->edge=(struct efs_client_lease_request){.ino=42,.generation=99,.flags=EFS_LEASE_EDGE_CLOSE,.active=1};
 g_open_refs->edge.q.id.seq=55;g_open_refs->retry_order=efs_open_retry_order(g_open_refs);
 newer=calloc(1,sizeof(*newer));assert(newer);
 newer->ino=43;newer->close_pending=1;newer->leased=1;
 newer->lease_generation=99;newer->retry_order=99;
 newer->edge=(struct efs_client_lease_request){.ino=43,.generation=99,.flags=EFS_LEASE_EDGE_CLOSE,.active=1};
 newer->edge.q.id.seq=70;newer->next=g_open_refs;g_open_refs=newer;
 assert(efs_open_drain_pending()==EFS_ERR_NET&&g_open_refs==newer&&newer->next&&server_hold);
 assert(efs_open_drain_pending()==EFS_ERR_BUSY&&g_open_refs->ino==42&&!g_open_refs->next&&server_hold);
 close_rc=0;assert(efs_open_drain_pending()==EFS_ERR_BUSY&&!g_open_refs&&!server_hold);

 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 efs_open_retry_start();assert(g_open_retry_started);usleep(200000);efs_open_retry_stop();
 assert(!g_open_refs&&!server_hold&&!g_open_retry_started);
 assert(!efs_open_acquire(42));int handed_off=0;
 adopt_rc=EFS_ERR_NET;int old_closes=closes;
 assert(efs_open_adopt(42,99,3,&handed_off)==EFS_ERR_NET&&handed_off&&g_open_refs->adoption.active);
 assert(efs_close_note(42,&leased));
 assert(efs_open_release_pending(42)==EFS_ERR_NET&&closes==old_closes&&server_hold);
 assert(efs_open_acquire(42)==EFS_ERR_NET&&closes==old_closes);
 adopt_rc=0;assert(!efs_open_release_pending(42)&&!g_open_refs&&!server_hold&&adopts>=2);
 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 close_rc=EFS_ERR_STALE;assert(efs_open_release_pending(42)==EFS_ERR_STALE && g_open_refs);
 revocation=1;assert(!efs_open_release_pending(42) && !g_open_refs && !server_hold);
 close_rc=0;revocation=0;assert(!efs_open_acquire(42));
 adopt_rc=EFS_ERR_NET;assert(efs_open_adopt(42,99,3,&handed_off)==EFS_ERR_NET);
 assert(efs_close_note(42,&leased));adopt_rc=EFS_ERR_STALE;close_rc=EFS_ERR_STALE;revocation=1;
 assert(!efs_open_release_pending(42) && !g_open_refs && !server_hold);
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
 /* One combined edge is enough for a first retained CREATE. */
 int wire_before=opens,adopt_before=adopts;handed_off=0;
 assert(!efs_open_acquire_created(42,99,3,&handed_off)&&handed_off);
 assert(opens==wire_before+1 && adopts==adopt_before+1 && g_open_refs->n==1);
 assert(!g_open_refs->adoption.active && g_open_refs->leased);
 /* Existing normal ownership receives only CLEAR, preserving its descriptor. */
 wire_before=opens;adopt_before=adopts;
 assert(!efs_open_acquire_created(42,99,3,&handed_off)&&handed_off);
 assert(opens==wire_before && adopts==adopt_before+1 && g_open_refs->n==2);
 assert(!efs_close_note(42,&leased));assert(efs_close_note(42,&leased));
 assert(!efs_open_release_pending(42)&&!g_open_refs&&!server_hold);
 /* Lost combined ACK retains its exact identity; reopen cannot pass it. */
 open_rc=EFS_ERR_NET;wire_before=opens;int old_adopt_begins=adopt_begins;
 assert(efs_open_acquire_created(42,99,3,&handed_off)==EFS_ERR_NET&&handed_off);
 assert(g_open_refs&&!g_open_refs->n&&g_open_refs->adoption.active);
 assert(g_open_refs->adoption.flags==(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING));
 assert(efs_open_acquire(42)==EFS_ERR_NET && opens==wire_before+1 && adopt_begins==old_adopt_begins+1);
 open_rc=0;assert(!efs_open_release_pending(42)&&!g_open_refs&&!server_hold);
 /* Admission refusal allocates no edge; retry releases the stripe. */
 adopt_begin_busy=2;wire_before=opens;old_adopt_begins=adopt_begins;
 assert(!efs_open_acquire_created(42,99,3,&handed_off)&&handed_off);
 assert(opens==wire_before+1&&adopt_begins==old_adopt_begins+3&&!pressure_sleep);
 assert(efs_close_note(42,&leased));assert(!efs_open_release_pending(42));
 /* A proved negative combined edge acquired no normal lease. Preserve the
  * original CREATE token for pending-only cleanup; do not send a fictional CLOSE. */
 combined_known_stale=1;old_closes=closes;handed_off=1;
 assert(efs_open_acquire_created(42,99,3,&handed_off)==EFS_ERR_STALE);
 assert(handed_off&&!g_open_refs&&!server_hold&&closes==old_closes);
 /* Negative proof learned after an unknown reply transfers pending cleanup
  * to a distinct CLEAR identity. Admission failure keeps the old token body. */
 combined_stale_unknown=1;combined_clear_begin_busy=1;old_closes=closes;
 assert(efs_open_acquire_created(42,99,3,&handed_off)==EFS_ERR_NET&&handed_off);
 assert(g_open_refs && !g_open_refs->n && !g_open_refs->leased && g_open_refs->adoption.complete);
 assert(g_open_refs->adoption.flags==(EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING));
 assert(g_open_refs->adoption.pending_token==3 && g_open_refs->adoption.q.id.seq==55);
 assert(g_open_refs->adoption.generation==100 && g_open_refs->lease_generation==99);
 assert(!efs_open_release_pending(42)&&!g_open_refs&&!server_hold&&closes==old_closes);
 combined_stale_unknown=0;combined_known_stale=0;
 mock_deadline=stats_now_ms()-1;handed_off=1;
 assert(efs_open_acquire_created(42,99,3,&handed_off)==EFS_ERR_BUSY&&!handed_off&&!g_open_refs);
 assert(mock_deadline && mock_deadline<stats_now_ms());mock_deadline=0;
 assert(efs_open_acquire_created(42,0,3,&handed_off)==EFS_ERR_INVAL);
 assert(efs_open_acquire_created(42,99,0,&handed_off)==EFS_ERR_INVAL);
 puts("FUSE ambiguous OPEN/CLOSE: retained ownership, reopen barrier and clean-stop retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-retention-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)

 # Reject the old scheduling key that never published newly allocated CLOSE.
 legacy=re.sub(r'^            r->retry_order = efs_open_retry_order\(r\);$',
               '            (void)efs_open_retry_order(r);',source,flags=re.M)
 assert legacy!=source
 p.write_text(legacy)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 rejected=subprocess.run([str(p.with_suffix(''))],capture_output=True,timeout=15)
 assert rejected.returncode!=0,'stale cleanup scheduling key must fail'
 print('unallocated CLOSE starvation negative control rejected PASS')
