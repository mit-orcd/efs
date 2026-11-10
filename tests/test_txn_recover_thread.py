#!/usr/bin/env python3
"""K106 (c): stranded-transaction recovery does not run behind the GC pass.

The GC pass ends in a reclaim step that waits for every member's acceptance; with
one member halted (SIGSTOP, a hung host) that step took 29 to 30 s and recovery,
the last stage of the same pass, ran 29 to 30 s late instead of 5 s after the
record's age. Recovery now has its own thread: production host_recover_thread
runs on the GC cadence while the GC thread is stuck, and stops at once."""
from pathlib import Path
import os,shlex,subprocess,tempfile
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('raft_host')
gc=s[s.index('static void *host_gc_thread('):s.index('static void *host_recover_thread(')]
assert 'host_txn_recover_pass(' not in gc, 'recovery must not run on the GC thread'
a=s.index('static void *host_recover_thread(');b=s.index('/* W92 query publication:')
assert 'efsd_pthread_create(&h->rec_tid, host_recover_thread, h)' in s
assert 'pthread_join(h->rec_tid, NULL)' in s
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define GC_LOOP_MS 1000
struct efs_raft_host {int gc_running;pthread_mutex_t rec_mu;pthread_cond_t rec_cv;};
static int calls;
static void host_txn_recover_pass(struct efs_raft_host *h){(void)h;__atomic_fetch_add(&calls,1,__ATOMIC_SEQ_CST);}
'''+s[a:b]+r'''
static int64_t ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int main(void){
 struct efs_raft_host h={.gc_running=1,.rec_mu=PTHREAD_MUTEX_INITIALIZER,.rec_cv=PTHREAD_COND_INITIALIZER};
 pthread_t t;assert(!pthread_create(&t,NULL,host_recover_thread,&h));
 int64_t t0=ms();
 while(__atomic_load_n(&calls,__ATOMIC_SEQ_CST)<2&&ms()-t0<5000)usleep(5000);
 int64_t cadence=ms()-t0;
 assert(__atomic_load_n(&calls,__ATOMIC_SEQ_CST)>=2&&cadence>=900&&cadence<3000); /* one pass per GC_LOOP_MS, no GC thread involved */
 t0=ms();
 __atomic_store_n(&h.gc_running,0,__ATOMIC_RELEASE);
 pthread_mutex_lock(&h.rec_mu);pthread_cond_broadcast(&h.rec_cv);pthread_mutex_unlock(&h.rec_mu);
 assert(!pthread_join(t,NULL)&&ms()-t0<500);                    /* shutdown does not wait out the cadence */
 puts("txn recovery thread: own cadence, prompt stop PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-recover-thread-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
