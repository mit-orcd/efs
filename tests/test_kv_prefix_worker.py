#!/usr/bin/env python3
"""Captured production WAL worker: overlap, exact proof, credits and drain."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.getenv('EFS_KV_PREFIX_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('struct host_kv_worker {');b=s.index('static void *host_pump(',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#define HOST_NGROUPS 2
#define BO_ON() 0
#define BH_KV_RELEASE_US 0
#define BO_HIST(k,n) ((void)(k),(void)(n))
static uint64_t efs_batch_obs_now_us(void){return 0;}
static uint64_t now_us_(void){return 1;}
struct efs_raft_host {void *kv;int flow_enabled;};
struct host_checkpoint_worker {int started;};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int paused,entered,sync_rc,detach_rc,create_fail,detaches,start_paused;
static void *(*start_fn)(void*);static void *start_arg;
static void *controlled_start(void *ignored){(void)ignored;pthread_mutex_lock(&gate);
 while(start_paused){pthread_cond_wait(&cv,&gate);}
 pthread_mutex_unlock(&gate);return start_fn(start_arg);}
static uint64_t appended,synced,published;
static unsigned credits,markers,coalescing_waits;
static int actual_cover;
#ifdef __APPLE__
static int counted_wait(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *t){coalescing_waits++;return pthread_cond_timedwait_relative_np(cv,mu,t);}
#define pthread_cond_timedwait_relative_np counted_wait
#else
static int counted_wait(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *t){coalescing_waits++;return pthread_cond_timedwait(cv,mu,t);}
#define pthread_cond_timedwait counted_wait
#endif
int efs_kv_lsm_sync_covered(void *kv,uint64_t p,int *covered){(void)kv;pthread_mutex_lock(&gate);*covered=!sync_rc&&p<=synced;int rc=sync_rc;pthread_mutex_unlock(&gate);return rc;}
static int efsd_pthread_create(pthread_t *t,void *(*f)(void*),void *p){if(create_fail)return 1;start_fn=f;start_arg=p;return pthread_create(t,NULL,controlled_start,NULL);}
static int efs_kv_lsm_sync_detach(void *kv,uint64_t *p){(void)kv;pthread_mutex_lock(&gate);*p=appended;detaches++;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);return detach_rc;}
static int efs_kv_lsm_sync_prefix(void *kv,uint64_t p){(void)kv;
 pthread_mutex_lock(&gate);
 if(p<=synced){int rc=sync_rc;pthread_mutex_unlock(&gate);return rc;}
 uint64_t target=actual_cover?appended:p;
 entered++;pthread_cond_broadcast(&cv);
 while(paused)pthread_cond_wait(&cv,&gate);
 int rc=sync_rc;if(!rc)synced=target;pthread_mutex_unlock(&gate);return rc;}
static void host_publish_cursor(struct efs_raft_host *h,const uint64_t *c,int rc){(void)h;
 if(!rc){assert(synced>=c[0]);published=c[0];}}
static int host_checkpoint_retire(struct efs_raft_host *h,const uint64_t *idx,uint64_t now,int rc,unsigned n,uint64_t bytes){
 (void)h;(void)now;(void)bytes;if(!rc){assert(published>=idx[0]);markers++;}credits+=n;return rc;}
static int host_checkpoint_dispatch(struct host_checkpoint_worker *w,const uint64_t *idx,uint64_t now,int rc,unsigned n,uint64_t bytes){
 (void)w;return host_checkpoint_retire(NULL,idx,now,rc,n,bytes);}
''' + s[a:b] + r'''
static void idle(struct host_kv_worker *w){pthread_mutex_lock(&w->mu);while(w->busy)pthread_cond_wait(&w->cv,&w->mu);pthread_mutex_unlock(&w->mu);}
static void wait_entered(int n){pthread_mutex_lock(&gate);while(entered<n)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);}
static void release(void){pthread_mutex_lock(&gate);paused=0;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);}
struct submission {struct host_kv_worker *w;uint64_t prefix[2];int rc,owned;};
static void *submit(void *p){struct submission *s=p;s->rc=host_kv_dispatch(s->w,s->prefix,s->prefix,2,30,&s->owned);return NULL;}
int main(void){
 struct efs_raft_host h={.flow_enabled=1};struct host_checkpoint_worker checkpoint={.started=1};struct host_kv_worker w;
 create_fail=1;assert(host_kv_start(&w,&h,&checkpoint)==EFS_ERR_IO&&!w.started);assert(!host_kv_stop(&w));create_fail=0;
 assert(!host_kv_start(&w,&h,&checkpoint));uint64_t prefix[2]={10,10};int owned=-1;
 appended=10;paused=1;assert(!host_kv_dispatch(&w,prefix,prefix,3,20,&owned)&&owned);wait_entered(1);
 assert(!synced&&!published&&!credits&&!markers);
 /* A read-only cycle retains the in-flight proof, without blocking on I/O. */
 assert(!host_kv_dispatch(&w,prefix,prefix,0,0,&owned)&&owned);
 /* New records do not expand the accepted job's captured prefix. */
 appended=20;struct submission second={.w=&w,.prefix={20,20}};pthread_t submitter;
 assert(!pthread_create(&submitter,NULL,submit,&second));
 pthread_mutex_lock(&gate);while(detaches<3)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);
 pthread_mutex_lock(&w.mu);assert(w.busy&&w.prefix==10&&w.idx[0]==10);pthread_mutex_unlock(&w.mu);
 assert(!synced&&!published&&!credits);release();pthread_join(submitter,NULL);idle(&w);
 assert(!second.rc&&second.owned&&synced==20&&published==20&&credits==5&&markers==2);
 prefix[0]=prefix[1]=20;
 /* Detach refusal retains caller ownership; no duplicate retirement. */
 detach_rc=EFS_ERR_BUSY;assert(host_kv_dispatch(&w,prefix,prefix,4,0,&owned)==EFS_ERR_BUSY&&!owned);assert(credits==5);detach_rc=0;
 /* Failed barriers never publish a new cursor or checkpoint. */
 sync_rc=EFS_ERR_IO;appended=30;prefix[0]=prefix[1]=30;
 assert(!host_kv_dispatch(&w,prefix,prefix,1,0,&owned)&&owned);idle(&w);
 assert(published==20&&markers==2&&credits==6);assert(host_kv_stop(&w)==EFS_ERR_IO);
 /* Drain completes an accepted successful job before destruction. */
 sync_rc=0;assert(!host_kv_start(&w,&h,&checkpoint));assert(!host_kv_dispatch(&w,prefix,prefix,1,0,&owned)&&owned);
 assert(!host_kv_stop(&w)&&published==30&&credits==7);
 /* Control credits arriving before argument capture belong to that job;
  * a subsequent data prefix wakes the coalescing window without overwrite. */
 start_paused=1;paused=0;assert(!host_kv_start(&w,&h,&checkpoint));
 appended=40;prefix[0]=prefix[1]=40;unsigned old_credits=credits,old_markers=markers;
 assert(!host_kv_dispatch(&w,prefix,prefix,2,20,&owned)&&owned);
 assert(!host_kv_dispatch(&w,prefix,prefix,3,30,&owned)&&owned);
 pthread_mutex_lock(&w.mu);assert(w.n==5&&!w.flushing&&w.prefix==40);pthread_mutex_unlock(&w.mu);
 appended=50;second=(struct submission){.w=&w,.prefix={50,50}};
 pthread_mutex_lock(&gate);int target_detaches=detaches+1;pthread_mutex_unlock(&gate);
 assert(!pthread_create(&submitter,NULL,submit,&second));
 pthread_mutex_lock(&gate);while(detaches<target_detaches)pthread_cond_wait(&cv,&gate);
 start_paused=0;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);
 pthread_join(submitter,NULL);assert(!second.rc&&second.owned);
 assert(!host_kv_stop(&w)&&published==50&&credits==old_credits+7&&markers==old_markers+2);
 /* An idle data tail really uses the bounded monotonic window. */
 assert(!host_kv_start(&w,&h,&checkpoint));appended=60;prefix[0]=prefix[1]=60;
 struct timespec before_time,after_time;clock_gettime(CLOCK_MONOTONIC,&before_time);
 assert(!host_kv_dispatch(&w,prefix,prefix,1,0,&owned)&&owned);idle(&w);
 clock_gettime(CLOCK_MONOTONIC,&after_time);
 int64_t elapsed=(int64_t)(after_time.tv_sec-before_time.tv_sec)*1000000000L+after_time.tv_nsec-before_time.tv_nsec;
 assert(elapsed>=500000L&&elapsed<2000000000L&&published==60);
 assert(!host_kv_stop(&w));
 /* Control reuse owns no duplicate credits or wait, but must wake data
  * not yet durable rather than adding an idle delay to a waiting read. */
 h.flow_enabled=0;start_paused=1;assert(!host_kv_start(&w,&h,&checkpoint));
 appended=70;prefix[0]=prefix[1]=70;
 assert(!host_kv_dispatch(&w,prefix,prefix,2,20,&owned)&&owned);
 assert(!host_kv_dispatch(&w,prefix,prefix,4,40,&owned)&&owned);
 pthread_mutex_lock(&w.mu);assert(w.n==2&&w.ready&&!w.flushing);pthread_mutex_unlock(&w.mu);
 pthread_mutex_lock(&gate);start_paused=0;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);
 assert(!host_kv_stop(&w)&&published==70);
 /* A prior barrier starts after the next cycle appended: its physical
  * frontier covers that next prefix. The next marker must not pay another
  * idle coalescing window. Cursor publication remains per captured job. */
 h.flow_enabled=1;actual_cover=1;start_paused=1;
 assert(!host_kv_start(&w,&h,&checkpoint));
 appended=80;prefix[0]=prefix[1]=80;
 unsigned before_waits=coalescing_waits;
 assert(!host_kv_dispatch(&w,prefix,prefix,1,0,&owned)&&owned);
 appended=90;second=(struct submission){.w=&w,.prefix={90,90}};
 assert(!pthread_create(&submitter,NULL,submit,&second));
 pthread_mutex_lock(&w.mu);while(!w.ready)pthread_cond_wait(&w.cv,&w.mu);pthread_mutex_unlock(&w.mu);
 pthread_mutex_lock(&gate);start_paused=0;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);
 assert(!pthread_join(submitter,NULL));idle(&w);
 assert(!second.rc&&second.owned&&published==90&&synced==90);
 assert(coalescing_waits==before_waits);
 assert(!host_kv_stop(&w));
 /* A physically covered control tail needs neither duplicate credits nor
  * a wake. This remains true after reopening the bounded worker. */
 h.flow_enabled=0;actual_cover=0;start_paused=1;
 assert(!host_kv_start(&w,&h,&checkpoint));
 prefix[0]=prefix[1]=90;
 assert(!host_kv_dispatch(&w,prefix,prefix,2,0,&owned)&&owned);
 assert(!host_kv_dispatch(&w,prefix,prefix,4,0,&owned)&&owned);
 pthread_mutex_lock(&w.mu);assert(w.n==2&&!w.ready&&!w.flushing);pthread_mutex_unlock(&w.mu);
 pthread_mutex_lock(&gate);start_paused=0;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);
 assert(!host_kv_stop(&w)&&published==90);
 /* A slow barrier must not hold up ordinary Raft receive cycles. One
  * bounded pending frontier covers many cycles while the active proof stays
  * immutable. No cursor/checkpoint may advance before its own barrier. */
 h.flow_enabled=0;actual_cover=0;paused=1;
 assert(!host_kv_start(&w,&h,&checkpoint));
 appended=100;prefix[0]=prefix[1]=100;
 old_credits=credits;old_markers=markers;int old_entered=entered;
 assert(!host_kv_dispatch(&w,prefix,prefix,1,10,&owned)&&owned);
 wait_entered(old_entered+1);
 appended=110;prefix[0]=prefix[1]=110;
 alarm(3); /* old blocking dispatch cannot make progress before release */
 assert(!host_kv_dispatch(&w,prefix,prefix,2,20,&owned)&&owned);
 appended=120;prefix[0]=prefix[1]=120;
 assert(!host_kv_dispatch(&w,prefix,prefix,3,30,&owned)&&owned);alarm(0);
 pthread_mutex_lock(&w.mu);
 assert(w.flushing&&w.prefix==100&&w.cursor[0]==100&&w.pending&&
        w.next_prefix==120&&w.next_cursor[0]==120&&w.next_n==5&&w.next_bytes==50);
 pthread_mutex_unlock(&w.mu);
 assert(published==90&&markers==old_markers&&credits==old_credits);
 release();assert(!host_kv_stop(&w));
 assert(published==120&&synced==120&&markers==old_markers+2&&credits==old_credits+6);
 /* Failure of the first prefix refuses both its cursor and a queued later
  * marker. Shutdown must still drain the fixed pending slot. */
 assert(!host_kv_start(&w,&h,&checkpoint));paused=1;
 appended=130;prefix[0]=prefix[1]=130;old_entered=entered;
 assert(!host_kv_dispatch(&w,prefix,prefix,1,0,&owned)&&owned);wait_entered(old_entered+1);
 appended=140;prefix[0]=prefix[1]=140;
 assert(!host_kv_dispatch(&w,prefix,prefix,2,0,&owned)&&owned);
 sync_rc=EFS_ERR_IO;old_markers=markers;release();
 assert(host_kv_stop(&w)==EFS_ERR_IO&&published==120&&markers==old_markers);
 puts("KV prefix worker: captured barrier, bounded pending frontier, overlap, failure refusal, credits and shutdown PASS");
}
'''
# Keep the old single-slot source executable for the negative regression:
# the deadline exposes its blocking dispatch before newer-slot introspection.
if 'int pending;' not in s:
    code=code.replace('assert(w.flushing&&w.prefix==100&&w.cursor[0]==100&&w.pending&&\n        w.next_prefix==120&&w.next_cursor[0]==120&&w.next_n==5&&w.next_bytes==50);',
                      'assert(w.flushing&&w.prefix==100&&w.cursor[0]==100);')
with tempfile.TemporaryDirectory(prefix='efs-kv-prefix-worker-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
