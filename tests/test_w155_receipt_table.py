#!/usr/bin/env python3
"""W155 receipt table (src/common/request_receipt.c): replay, join, re-run, eviction.

A duplicate of an applied request is answered from the receipt without
running; a duplicate of a running attempt joins it; a running attempt that
outlives the join wait answers BUSY; an attempt that ended with an unknown
verdict is run again (same identity); outcomes only ever rise; eviction
takes finished receipts before a running one. Threads use real, short waits; the
asserted outcomes do not depend on scheduling."""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sanitize_env import cflags, timeout, wrap  # noqa: E402

root = Path(__file__).resolve().parents[1]
code = r'''
#include "efs/request_receipt.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static struct efs_receipt_table t;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int execs;
static int next_rc=EFS_OK,next_proposed=1;
struct arg{struct efs_receipt_key k;uint32_t join;int hold,entered,how,rc;struct efs_receipt r;};
/* ctx: the runner's arg; a held runner waits in exec until released. */
static int exec_fn(void *ctx,int *proposed,const char **phase){struct arg *a=ctx;
 pthread_mutex_lock(&mu);execs++;
 if(a){a->entered=1;pthread_cond_broadcast(&cv);while(a->hold)pthread_cond_wait(&cv,&mu);}
 int rc=next_rc;*proposed=next_proposed;pthread_mutex_unlock(&mu);
 *phase=rc?"wait":NULL;return rc;}
static struct efs_receipt_key key(uint64_t seq){struct efs_receipt_key k;memset(&k,0,sizeof(k));k.uuid[0]=0xab;k.epoch=2;k.mint_ns=99;k.seq=seq;return k;}
static void *runner(void *p){struct arg *a=p;a->rc=efs_receipt_run(&t,&a->k,42,3,a->join,exec_fn,a,&a->how,&a->r);return NULL;}
static void wait_entered(struct arg *a){pthread_mutex_lock(&mu);while(!a->entered)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);}
static void release(struct arg *a){pthread_mutex_lock(&mu);a->hold=0;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);}
static void reset(void){execs=0;next_rc=EFS_OK;next_proposed=1;}
int main(void){
 assert(!efs_receipt_table_init(&t,16));
 /* outcome and merge */
 assert(efs_receipt_outcome(EFS_OK,0)==EFS_RECEIPT_OUT_APPLIED);
 assert(efs_receipt_outcome(EFS_ERR_BUSY,0)==EFS_RECEIPT_OUT_NONE);
 assert(efs_receipt_outcome(EFS_ERR_STALE,1)==EFS_RECEIPT_OUT_PARTIAL);
 assert(efs_receipt_outcome(EFS_ERR_BUSY,1)==EFS_RECEIPT_OUT_UNKNOWN);
 assert(efs_receipt_merge(EFS_RECEIPT_OUT_UNKNOWN,EFS_RECEIPT_OUT_NONE)==EFS_RECEIPT_OUT_UNKNOWN);
 assert(efs_receipt_merge(EFS_RECEIPT_OUT_NONE,EFS_RECEIPT_OUT_APPLIED)==EFS_RECEIPT_OUT_APPLIED);
 struct efs_receipt r;int how;struct efs_receipt_key k=key(1);
 assert(!efs_receipt_lookup(&t,&k,&r));
 /* ran, then a duplicate is replayed without running */
 reset();assert(efs_receipt_run(&t,&k,42,3,100,exec_fn,NULL,&how,&r)==EFS_OK && how==EFS_RECEIPT_RAN && execs==1);
 assert(r.state==EFS_RECEIPT_STATE_DONE && r.outcome==EFS_RECEIPT_OUT_APPLIED && r.attempts==1 && r.ino==42 && r.nrec==3);
 assert(efs_receipt_run(&t,&k,42,3,100,exec_fn,NULL,&how,&r)==EFS_OK && how==EFS_RECEIPT_REPLAY && execs==1 && r.replayed==1);
 assert(efs_receipt_lookup(&t,&k,&r) && r.outcome==EFS_RECEIPT_OUT_APPLIED);
 /* unknown verdict: the same identity runs again; outcomes only rise */
 reset();k=key(2);next_rc=EFS_ERR_BUSY;
 assert(efs_receipt_run(&t,&k,42,3,100,exec_fn,NULL,&how,&r)==EFS_ERR_BUSY && r.outcome==EFS_RECEIPT_OUT_UNKNOWN && !strcmp(r.phase,"wait"));
 next_rc=EFS_ERR_BUSY;next_proposed=0;
 assert(efs_receipt_run(&t,&k,42,3,100,exec_fn,NULL,&how,&r)==EFS_ERR_BUSY && how==EFS_RECEIPT_RAN && r.attempts==2 && r.outcome==EFS_RECEIPT_OUT_UNKNOWN);
 next_rc=EFS_OK;assert(efs_receipt_run(&t,&k,42,3,100,exec_fn,NULL,&how,&r)==EFS_OK && r.attempts==3 && r.outcome==EFS_RECEIPT_OUT_APPLIED && execs==3);
 /* a duplicate of a running attempt joins it and takes its result */
 reset();pthread_t a1,a2;struct arg x={.k=key(3),.join=5000,.hold=1},y={.k=key(3),.join=5000};
 assert(!pthread_create(&a1,NULL,runner,&x));wait_entered(&x);
 assert(!pthread_create(&a2,NULL,runner,&y));
 for(int i=0;i<200;i++){struct efs_receipt q;assert(efs_receipt_lookup(&t,&x.k,&q)&&q.state==EFS_RECEIPT_STATE_PENDING);usleep(1000);}
 release(&x);pthread_join(a1,NULL);pthread_join(a2,NULL);
 assert(x.rc==EFS_OK && x.how==EFS_RECEIPT_RAN && y.rc==EFS_OK && (y.how==EFS_RECEIPT_JOINED||y.how==EFS_RECEIPT_REPLAY) && execs==1);
 /* the running attempt outlives the join wait: BUSY, nothing run twice */
 reset();x=(struct arg){.k=key(4),.join=5000,.hold=1};y=(struct arg){.k=key(4),.join=20};
 assert(!pthread_create(&a1,NULL,runner,&x));wait_entered(&x);
 assert(!pthread_create(&a2,NULL,runner,&y));pthread_join(a2,NULL);
 assert(y.rc==EFS_ERR_BUSY && y.how==EFS_RECEIPT_WAITING && y.r.state==EFS_RECEIPT_STATE_PENDING && execs==1);
 release(&x);pthread_join(a1,NULL);assert(x.rc==EFS_OK && execs==1);
 /* the joined attempt ended unknown: the joiner runs it itself */
 reset();next_rc=EFS_ERR_BUSY;x=(struct arg){.k=key(5),.join=5000,.hold=1};y=(struct arg){.k=key(5),.join=5000};
 assert(!pthread_create(&a1,NULL,runner,&x));wait_entered(&x);
 assert(!pthread_create(&a2,NULL,runner,&y));usleep(20000);
 release(&x);pthread_join(a1,NULL);
 pthread_join(a2,NULL);assert(x.rc==EFS_ERR_BUSY && y.how==EFS_RECEIPT_RAN && execs==2 && y.r.attempts==2);
 /* the joined attempt ended partial: the joiner gets the same STALE */
 reset();next_rc=EFS_ERR_STALE;x=(struct arg){.k=key(6),.join=5000,.hold=1};y=(struct arg){.k=key(6),.join=5000};
 assert(!pthread_create(&a1,NULL,runner,&x));wait_entered(&x);
 assert(!pthread_create(&a2,NULL,runner,&y));usleep(20000);release(&x);pthread_join(a1,NULL);pthread_join(a2,NULL);
 assert(x.rc==EFS_ERR_STALE && (y.how==EFS_RECEIPT_JOINED ? y.rc==EFS_ERR_STALE && execs==1 : y.how==EFS_RECEIPT_RAN));
 /* bounded: 1000 identities in 16 slots evict the oldest finished ones */
 reset();for(uint64_t s=100;s<1100;s++){struct efs_receipt_key e=key(s);assert(!efs_receipt_run(&t,&e,1,1,10,exec_fn,NULL,&how,&r));}
 { struct efs_receipt_key e=key(1099),o=key(100);assert(efs_receipt_lookup(&t,&e,&r));assert(!efs_receipt_lookup(&t,&o,&r));assert(t.evictions>0); }
 /* a running attempt keeps its receipt through a full table of newer ones */
 reset();x=(struct arg){.k=key(5000),.join=10,.hold=1};
 assert(!pthread_create(&a1,NULL,runner,&x));wait_entered(&x);
 for(uint64_t s=6000;s<7000;s++){struct efs_receipt_key e=key(s);(void)efs_receipt_run(&t,&e,1,1,10,exec_fn,NULL,&how,&r);}
 /* finished receipts are evicted first: the running one keeps its slot */
 { struct efs_receipt_key o=key(5000);assert(efs_receipt_lookup(&t,&o,&r) && r.state==EFS_RECEIPT_STATE_PENDING); }
 release(&x);pthread_join(a1,NULL);
 assert(efs_receipt_lookup(&t,&x.k,&r) && r.state==EFS_RECEIPT_STATE_DONE && r.outcome==EFS_RECEIPT_OUT_APPLIED);
 char id[64];struct efs_receipt_key f=key(7);efs_receipt_key_format(&f,id,sizeof(id));assert(!strcmp(id,"ab000000/2/63:7"));
 efs_receipt_table_free(&t);
 puts("W155 receipt table: replay, re-run after unknown, join, join timeout BUSY, joined unknown re-runs, joined partial, eviction of finished receipts with the running one kept PASS");
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-w155-table-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'), *cflags(),
                         str(p), str(root / 'src/common/request_receipt.c'), '-pthread',
                         '-o', str(p.with_suffix(''))], check=True)
    subprocess.run(wrap() + [str(p.with_suffix(''))], check=True, timeout=timeout(60))
