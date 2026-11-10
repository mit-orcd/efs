#!/usr/bin/env python3
"""K106 (c): stranded-transaction recovery does not run behind the GC pass.

The GC pass ends in a reclaim step that waits for every member's acceptance; with
one member halted (SIGSTOP, a hung host) that step took 29 to 30 s and recovery,
the last stage of the same pass, ran 29 to 30 s late instead of 5 s after the
record's age. Recovery now has its own thread: production host_recover_thread
runs on the GC cadence while the GC thread is stuck, paced on the monotonic clock
(a wall-clock step moves nothing), and the stop broadcast ends its wait. No test
bound is a duration: a fake clock and a fake timed wait observe the deadline asked
for, and a hang is the harness timeout."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('raft_host')
gc=s[s.index('static void *host_gc_thread('):s.index('static void *host_recover_thread(')]
assert 'host_txn_recover_pass(' not in gc, 'recovery must not run on the GC thread'
a=s.index('static void *host_recover_thread(');b=s.index('/* W92 query publication:')
assert 'efsd_pthread_create(&h->rec_tid, host_recover_thread, h)' in s
assert 'pthread_join(h->rec_tid, NULL)' in s
assert re.search(r'pthread_condattr_setclock\(&rec_attr,\s*CLOCK_MONOTONIC\);\s*pthread_cond_init\(&h->rec_cv,\s*&rec_attr\)',s),'rec_cv must run on the monotonic clock'
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define GC_LOOP_MS 1000
struct efs_raft_host {int gc_running;pthread_mutex_t rec_mu;pthread_cond_t rec_cv;};
static int calls,waits,parked;
static struct timespec seen[2];
/* A fixed clock: monotonic 1000.999 s, wall clock far away and different. */
static int fake_clock(clockid_t id,struct timespec *ts){
 if(id==CLOCK_MONOTONIC){ts->tv_sec=1000;ts->tv_nsec=999000000L;}else{ts->tv_sec=4000000000L;ts->tv_nsec=5;}
 return 0;}
/* The first two cadence waits time out at once and record the deadline asked
 * for; the third parks until the broadcast of the stop sequence. */
static int fake_wait(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *ts){
 int n=waits++;
 if(n<2){seen[n]=*ts;return ETIMEDOUT;}
 struct efs_raft_host *h=(struct efs_raft_host *)((char *)cv-__builtin_offsetof(struct efs_raft_host,rec_cv));
 __atomic_store_n(&parked,1,__ATOMIC_SEQ_CST);
 while(__atomic_load_n(&h->gc_running,__ATOMIC_ACQUIRE))pthread_cond_wait(cv,mu);
 return 0;}
#define clock_gettime fake_clock
#define pthread_cond_timedwait fake_wait
static void host_txn_recover_pass(struct efs_raft_host *h){(void)h;__atomic_fetch_add(&calls,1,__ATOMIC_SEQ_CST);}
'''+s[a:b]+r'''
int main(void){
 struct efs_raft_host h={.gc_running=1,.rec_mu=PTHREAD_MUTEX_INITIALIZER,.rec_cv=PTHREAD_COND_INITIALIZER};
 pthread_t t;assert(!pthread_create(&t,NULL,host_recover_thread,&h));
 while(!__atomic_load_n(&parked,__ATOMIC_SEQ_CST))usleep(1000);   /* a state, not a duration; the harness timeout bounds a hang */
 assert(calls==3&&waits==3);                                       /* a pass per cadence wait, no GC thread involved */
 for(int i=0;i<2;i++)assert(seen[i].tv_sec==1001&&seen[i].tv_nsec==999000000L); /* monotonic now + GC_LOOP_MS, nsec carried */
 __atomic_store_n(&h.gc_running,0,__ATOMIC_RELEASE);
 pthread_mutex_lock(&h.rec_mu);pthread_cond_broadcast(&h.rec_cv);pthread_mutex_unlock(&h.rec_mu);
 assert(!pthread_join(t,NULL));                                    /* the stop broadcast ends the parked wait */
 puts("txn recovery thread: own cadence on the monotonic clock, stop wakes the wait PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-recover-thread-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
