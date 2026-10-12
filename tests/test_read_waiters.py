#!/usr/bin/env python3
"""Fresh-read wake predicates and one notification per registered sleeper."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/server/raft_host.c').read_text()
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
#define V_LOAD(x) (x)
''' + structure('host_view') + structure('host_waiter') + r'''
struct host_group {int v_has; uint64_t v_stamp,v_applied;};
struct efs_raft_host {pthread_mutex_t cv_mu;struct host_waiter waiters[HOST_WAITERS];
 struct host_group group;struct host_view view;int cv_overflow;pthread_cond_t applied_cv;};
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){return group==0?&h->group:NULL;}
static void host_view_get(struct efs_raft_host *h,uint8_t group,struct host_view *v){(void)group;*v=h->view;}
static unsigned signals,broadcasts;
static int signal_mock(pthread_cond_t *cv){(void)cv;signals++;return 0;}
static int broadcast_mock(pthread_cond_t *cv){(void)cv;broadcasts++;return 0;}
#define pthread_cond_signal signal_mock
#define pthread_cond_broadcast broadcast_mock
''' + section('static int host_view_covers(', '/* Sleep until') + section('static void host_waiters_wake(', 'static int host_hosts(') + r'''
static void reset(struct efs_raft_host *h){
 h->waiters[0]=(struct host_waiter){.active=1,.any=2,.idx=50,.round=11,.term=9};
 h->view=(struct host_view){.has=1,.role=2,.term=9,.read_pending=1,.read_round=10,.applied=50,.commit=50};
 signals=0;
}
int main(void){
 struct efs_raft_host h={.cv_mu=PTHREAD_MUTEX_INITIALIZER,.group={.v_has=1}};
 reset(&h);
 for(unsigned i=0;i<1000;i++){h.view.commit++;h.group.v_stamp++;host_waiters_wake(&h);}
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
 reset(&h);h.view.term++;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.view.role=1;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.view.has=0;host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.group.v_has=0;host_waiters_wake(&h);assert(signals==1);h.group.v_has=1;
 reset(&h);h.waiters[0].any=0;h.waiters[0].idx=60;h.group.v_applied=59;
 host_waiters_wake(&h);assert(signals==0);h.group.v_applied=60;
 host_waiters_wake(&h);host_waiters_wake(&h);assert(signals==1);
 reset(&h);h.waiters[0].any=1;h.waiters[0].seen=h.group.v_stamp;
 host_waiters_wake(&h);assert(signals==0);h.group.v_stamp++;
 host_waiters_wake(&h);host_waiters_wake(&h);assert(signals==1);
 h.cv_overflow=1;host_waiters_wake(&h);assert(broadcasts==1);
 puts("read waiters: fresh proof, pending apply, old round, invalidation and single signal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-waiters-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
