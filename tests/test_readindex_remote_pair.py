#!/usr/bin/env python3
"""Actual bounded worker pool: overlap, deadline ownership, refusal and reuse."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=source.index('static unsigned host_read_pair_capacity(');capacity=source[a:source.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include "efs/network.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
struct efsd_server {int storage_engine;};
struct efs_raft_host {struct efsd_server *s;pthread_mutex_t mu;pthread_cond_t cv;unsigned entered[3];
 int wait,first_rc,second_rc,done,result,hint;uint64_t deadline;};
static __thread uint64_t scope;
uint64_t efs_net_deadline_ms(void){return scope;}
void efs_net_set_deadline_ms(uint64_t value){scope=value;}
static int creates,deny_after=100;
static int checked_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{
 size_t bytes;int detached;
 assert(!pthread_attr_getstacksize(a,&bytes) && bytes<=(128u<<10));
 assert(!pthread_attr_getdetachstate(a,&detached) && detached==PTHREAD_CREATE_DETACHED);
 creates++;if(creates>deny_after)return 11;
 return pthread_create(t,a,fn,arg);
}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint)
{
 assert((group==0 || group==2) && scope==h->deadline);
 pthread_mutex_lock(&h->mu);h->entered[group]++;
 pthread_cond_broadcast(&h->cv);
 while((h->wait==1 && (!h->entered[0] || !h->entered[2])) || (h->wait==2 && group==2))
  pthread_cond_wait(&h->cv,&h->mu);
 int rc=group==0?h->first_rc:h->second_rc;
 pthread_mutex_unlock(&h->mu);if(hint)*hint=group==0?11:22;return rc;
}
static struct efs_raft_host *g_host;
static unsigned parallel_notes,scalar_notes;
static void test_note(int parallel){if(parallel)__atomic_fetch_add(&parallel_notes,1,__ATOMIC_RELAXED);else __atomic_fetch_add(&scalar_notes,1,__ATOMIC_RELAXED);}
'''+capacity+r'''
#define HOST_READ_PAIR_LIMIT() host_read_pair_capacity()
#define HOST_READ_PAIR_NOTE(parallel) test_note(parallel)
#define pthread_create checked_create
'''+(root/'src/server/read_index_pair_remote.h').read_text()+r'''
#undef pthread_create
static void init(struct efs_raft_host *h,uint64_t deadline)
{*h=(struct efs_raft_host){.deadline=deadline};pthread_mutex_init(&h->mu,NULL);pthread_cond_init(&h->cv,NULL);}
static void finish(struct efs_raft_host *h)
{assert(!pthread_mutex_destroy(&h->mu));assert(!pthread_cond_destroy(&h->cv));}
static int run(struct efs_raft_host *h)
{
 scope=h->deadline;int rc=host_read_index_remote_pair(h,0,2,&h->hint);
 assert(scope==h->deadline && h->entered[0]==1);
 return rc;
}
static void *caller(void *ctx)
{struct efs_raft_host *h=ctx;h->result=run(h);__atomic_store_n(&h->done,1,__ATOMIC_RELEASE);return NULL;}
int main(int argc,char **argv)
{
 assert(HOST_READ_PAIR_WORKERS==32 && HOST_READ_PAIR_STACK<=(128u<<10));
 struct efsd_server server={argc>2?atoi(argv[2]):0};
 struct efs_raft_host fixture={.s=&server};g_host=&fixture;
 unsigned limit=server.storage_engine?32:8;assert(host_read_pair_capacity()==limit);
 g_host=NULL;assert(host_read_pair_capacity()==8);g_host=&fixture;
 if(argc>1)deny_after=atoi(argv[1]);
 struct efs_raft_host h;init(&h,777);h.wait=deny_after?1:0;
 assert(!run(&h) && h.entered[2]==1 && h.hint==22);finish(&h);
 unsigned count=host_read_pair_count;assert(count==(deny_after==0?0:deny_after==3?3:limit));
 int startup_creates=creates;
 /* Fill every real helper with a distinct caller/deadline; overflow uses
  * the scalar path immediately and every borrowed stack/host is joined. */
 if(count) {
  struct efs_raft_host hosts[HOST_READ_PAIR_WORKERS];pthread_t callers[HOST_READ_PAIR_WORKERS];
  for(unsigned i=0;i<count;i++) {
   init(&hosts[i],2000+i);hosts[i].wait=2;
   assert(!pthread_create(&callers[i],NULL,caller,&hosts[i]));
   pthread_mutex_lock(&hosts[i].mu);
   while(!hosts[i].entered[0] || !hosts[i].entered[2])pthread_cond_wait(&hosts[i].cv,&hosts[i].mu);
   pthread_mutex_unlock(&hosts[i].mu);
  }
  unsigned previous=scalar_notes;init(&h,3000);assert(!run(&h)&&h.entered[2]==1);finish(&h);
  assert(scalar_notes==previous+1);
  for(unsigned i=0;i<count;i++) {
   pthread_mutex_lock(&hosts[i].mu);assert(!__atomic_load_n(&hosts[i].done,__ATOMIC_ACQUIRE));
   hosts[i].wait=0;pthread_cond_broadcast(&hosts[i].cv);pthread_mutex_unlock(&hosts[i].mu);
   assert(!pthread_join(callers[i],NULL));assert(!hosts[i].result);finish(&hosts[i]);
  }
 }
 /* Reuse never borrows a previous caller's budget or starts new threads.
  * Destroying every host after its joined RPC proves no task escapes it. */
 for(unsigned i=0;i<200;i++) {
  init(&h,1000+i);h.wait=count?1:0;
  assert(!run(&h) && h.entered[2]==1);finish(&h);
 }
 assert(creates==startup_creates);
 /* Exhausted admission returns immediately to the scalar implementation. */
 pthread_mutex_lock(&host_read_pair_mu);
 for(unsigned i=0;i<count;i++){assert(!host_read_pair_workers[i].busy);host_read_pair_workers[i].busy=1;}
 pthread_mutex_unlock(&host_read_pair_mu);
 init(&h,888);assert(!run(&h) && h.entered[2]==1);finish(&h);
 init(&h,889);h.first_rc=EFS_ERR_NOT_PRIMARY;
 assert(run(&h)==EFS_ERR_NOT_PRIMARY && !h.entered[2] && h.hint==11);finish(&h);
 pthread_mutex_lock(&host_read_pair_mu);
 for(unsigned i=0;i<count;i++)host_read_pair_workers[i].busy=0;
 pthread_mutex_unlock(&host_read_pair_mu);
 if(count) {
  /* An early first-group failure cannot release stack/host ownership while
   * the second callback is still executing. No partial proof is returned. */
  init(&h,990);h.wait=2;h.first_rc=EFS_ERR_NOT_PRIMARY;
  pthread_t thread;assert(!pthread_create(&thread,NULL,caller,&h));
  pthread_mutex_lock(&h.mu);
  while(!h.entered[0] || !h.entered[2])pthread_cond_wait(&h.cv,&h.mu);
  assert(!__atomic_load_n(&h.done,__ATOMIC_ACQUIRE));h.wait=0;pthread_cond_broadcast(&h.cv);
  pthread_mutex_unlock(&h.mu);assert(!pthread_join(thread,NULL));
  assert(h.result==EFS_ERR_NOT_PRIMARY && h.hint==11);finish(&h);
 }
 init(&h,991);h.second_rc=EFS_ERR_IO;
 assert(run(&h)==EFS_ERR_IO && h.hint==22);finish(&h);
 pthread_mutex_lock(&host_read_pair_mu);
 for(unsigned i=0;i<count;i++)assert(!host_read_pair_workers[i].busy && !host_read_pair_workers[i].pending);
 pthread_mutex_unlock(&host_read_pair_mu);
 puts("remote pair: concurrent independent proofs, inherited budgets, bounded persistent stacks, saturation/init fallback, hard refusal and joined host/stack lifetime PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-remote-read-pair-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-pthread','-o',str(p/'test')],check=True)
    for args in ([],['0'],['3'],['100','1'],['0','1'],['3','1']):
        subprocess.run([str(p/'test')]+args,check=True,timeout=15)
