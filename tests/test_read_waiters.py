#!/usr/bin/env python3
"""Fresh-read wake predicates and one notification per registered sleeper."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
def section(start, end):
    a=s.index(start); return s[a:s.index(end,a)]
def structure(name):
    a=s.index('struct '+name+' {'); return s[a:s.index('\n};',a)+3]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#define EFS_RAFT_LEADER 2
#define HOST_WAITERS 512
#define HOST_NGROUPS 2
#define V_LOAD(x) (x)
''' + structure('host_view') + section('struct host_read_floor {', 'struct host_waiter {') + structure('host_waiter') + r'''
struct host_group {int v_has; uint64_t v_stamp,v_applied;};
struct efs_raft_host {pthread_mutex_t cv_mu;struct host_waiter waiters[HOST_WAITERS];
 struct host_group g[HOST_NGROUPS];struct host_view view,other_view;uint32_t waiter_hint;int cv_overflow;pthread_cond_t applied_cv;};
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){return group==0?&h->g[0]:group==2?&h->g[1]:NULL;}
static unsigned view_calls;
static void host_view_get(struct efs_raft_host *h,uint8_t group,struct host_view *v){view_calls++;*v=group? h->other_view:h->view;}
static unsigned signals,broadcasts;
static struct efs_raft_host *registration;
static int wait_mock(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *end){(void)end;assert(mu==&registration->cv_mu && cv==&registration->waiters[0].cv);struct host_waiter *w=&registration->waiters[0];assert(w->active && !w->notified && w->any==2 && w->idx==50 && w->round==11 && w->term==9 && w->proof_only);return 0;}
#define pthread_cond_timedwait wait_mock
static int signal_mock(pthread_cond_t *cv){(void)cv;signals++;return 0;}
static int broadcast_mock(pthread_cond_t *cv){(void)cv;broadcasts++;return 0;}
#define pthread_cond_signal signal_mock
#define pthread_cond_broadcast broadcast_mock
''' + section('static int host_view_covers(', '/* Sleep until') + section('static void host_waiter_sleep(', '/* Pump side:') + section('static void host_waiters_wake(', 'static int host_hosts(') + r'''
static void reset(struct efs_raft_host *h){
 h->waiters[0]=(struct host_waiter){.active=1,.any=2,.idx=50,.round=11,.term=9};
 h->view=(struct host_view){.has=1,.role=2,.term=9,.read_pending=1,.read_round=10,.applied=50,.commit=50};
 signals=0;view_calls=0;
}
int main(void){
 struct efs_raft_host h={.cv_mu=PTHREAD_MUTEX_INITIALIZER,.g={{.v_has=1}}};
 reset(&h);
 for(unsigned i=0;i<1000;i++){h.view.commit++;h.g[0].v_stamp++;host_waiters_wake(&h);}
 assert(signals==0); /* unrelated commits cannot complete a pending fresh proof */
 h.view.proven_done=1;h.view.proven_round=11;h.view.proven_index=50;
 for(unsigned i=0;i<1000;i++)host_waiters_wake(&h);
 assert(signals==1); /* no repeated signals before the waiter reacquires cv_mu */
 reset(&h);h.view.read_pending=0;host_waiters_wake(&h);assert(signals==1); /* old round done: start next */
 reset(&h);h.view.read_pending=0;h.view.read_done=1;h.view.read_round=11;h.view.read_index=60;
 host_waiters_wake(&h);assert(signals==0); /* fresh quorum still owes apply */
 h.view.applied=60;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.view.proven_done=1;h.view.proven_round=11;h.view.proven_index=60;
 h.view.read_pending=0;host_waiters_wake(&h);assert(signals==0);
 h.view.applied=60;host_waiters_wake(&h);assert(signals==1);
 /* The earlier experiment missed this: raw quorum complete but pending
  * own apply must wake a NEXT-round admission, not wait for the old apply. */
 reset(&h);h.view.read_done=1;h.view.read_round=10;h.view.read_index=60;h.view.applied=40;
 host_waiters_wake(&h);assert(signals==1);
 /* Same-round normal data remains apply-fenced, certificate replies do not. */
 reset(&h);h.view.read_done=1;h.view.read_round=11;h.view.read_index=60;
 host_waiters_wake(&h);assert(signals==0);
 h.waiters[0].proof_only=1;host_waiters_wake(&h);assert(signals==1);
 /* A proof for this reader is observed before own apply catches up.
  * An older retained proof and a newer raw proof must not replace its floor. */
 reset(&h);h.view.read_context=7;h.view.read_done=1;
 h.view.read_round=11;h.view.read_index=60;
 host_waiters_wake(&h);assert(!signals && h.waiters[0].floor.index==60);
 h.view.read_round=12;h.view.read_index=900;
 h.view.proven_done=1;h.view.proven_round=10;h.view.proven_index=55;h.view.applied=60;
 host_waiters_wake(&h);assert(signals==1 && h.waiters[0].floor.index==60);
 assert(host_read_floor_ready(&h.view,&h.waiters[0].floor,50,11,9,0));
 reset(&h);h.view.read_context=7;h.view.read_done=1;h.view.read_round=11;h.view.read_index=60;
 host_waiters_wake(&h);assert(!signals && h.waiters[0].floor.context==7);
 h.view.read_context=8;host_waiters_wake(&h);assert(signals==1); /* refuse changed cfg */
 reset(&h);h.view.read_context=7;h.view.read_done=1;h.view.read_round=11;h.view.read_index=60;
 host_waiters_wake(&h);h.view.read_context=0;host_waiters_wake(&h);assert(signals==1); /* storage failure */
 /* A scheduling wake for the next round is not completion. Capture its
  * later eligible proof even if that first signal has not been consumed. */
 reset(&h);h.waiters[0].floor.context=7;h.view.read_context=7;
 h.view.read_done=1;host_waiters_wake(&h);assert(signals==1&&!h.waiters[0].floor.index);
 h.view.read_round=11;h.view.read_index=60;host_waiters_wake(&h);
 assert(signals==1 && h.waiters[0].floor.index==60);
 h.view.read_round=12;h.view.read_index=900;h.view.applied=60;host_waiters_wake(&h);
 assert(signals==1 && h.waiters[0].floor.index==60);
 reset(&h);h.view.term++;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.view.role=1;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.view.has=0;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.g[0].v_has=0;host_waiters_wake(&h);assert(signals==1);h.g[0].v_has=1;
 reset(&h);h.waiters[0].any=0;h.waiters[0].idx=60;h.g[0].v_applied=59;
 host_waiters_wake(&h);assert(signals==0);h.g[0].v_applied=60;
 host_waiters_wake(&h);host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.waiters[0].any=1;h.waiters[0].seen=h.g[0].v_stamp;
 host_waiters_wake(&h);assert(signals==0);h.g[0].v_stamp++;
 host_waiters_wake(&h);host_waiters_wake(&h);assert(signals==1);
 h.cv_overflow=1;host_waiters_wake(&h);assert(broadcasts==1);
 /* Cache one coherent view per group instead of 512 separate snapshots. */
 reset(&h);h.cv_overflow=0;
 for(unsigned i=0;i<HOST_WAITERS;i++)h.waiters[i]=h.waiters[0];
 for(unsigned i=0;i<1000;i++){h.view.commit++;h.g[0].v_stamp++;host_waiters_wake(&h);}
 assert(!signals && view_calls==1000);
 h.view.proven_done=1;h.view.proven_round=11;h.view.proven_index=50;
 host_waiters_wake(&h);assert(signals==HOST_WAITERS && view_calls==1001);
 for(unsigned i=0;i<1000;i++)host_waiters_wake(&h);
 assert(signals==HOST_WAITERS && view_calls==1001);
 h.waiters[0].notified=0;h.waiters[1].notified=0;h.waiters[1].group=2;
 h.g[1].v_has=1;h.other_view=(struct host_view){.has=1,.role=2,.term=9,.read_pending=1,.read_round=10,.applied=50};
 unsigned before=view_calls;host_waiters_wake(&h);
 assert(signals==HOST_WAITERS+1 && view_calls==before+2 && !h.waiters[1].notified);
 /* Reused notified slots must reset registration before entering wait. */
 for(unsigned i=0;i<HOST_WAITERS;i++)h.waiters[i].active=0;
 h.waiter_hint=0;h.waiters[0].notified=1;registration=&h;
 struct timespec end={0};pthread_mutex_lock(&h.cv_mu);
 host_waiter_sleep(&h,0,2,50,0,&end,11,9,1,NULL);
 pthread_mutex_unlock(&h.cv_mu);assert(!h.waiters[0].active && !h.waiters[0].notified);
 puts("read waiters: fresh proof, pending apply, old round, invalidation and single signal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-waiters-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
