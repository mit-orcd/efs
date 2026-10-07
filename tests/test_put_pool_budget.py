#!/usr/bin/env python3
"""Production PUT pool: bounded queues, shutdown refusal and retryable startup."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M);return s[m.start():s.index('\n}',m.end())+2]
def block(name):
 a=s.index('struct '+name+' {');return s[a:s.index('\n};',a)+3]
a=s.index('#define PUT_POOL_QDEPTH');b=s.index('static void *put_pool_thread(',a)
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
'''+block('put_batch')+block('chunk_put_job')+'\n'+s[a:b]+r'''
#undef PUT_POOL_QDEPTH
#define PUT_POOL_QDEPTH 1
static __thread uint64_t deadline;
static unsigned writes,creates;static int fail_create;
static uint64_t report_clock_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
int efs_client_rpc_past_deadline(void) {return deadline&&report_clock_ms()>=deadline;}
static void *chunk_put_worker(void *p) {struct chunk_put_job *j=p;assert(deadline&&!efs_client_rpc_past_deadline());__sync_fetch_and_add(&writes,1);usleep(1000);j->rc=EFS_OK;return NULL;}
static int (*real_create)(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *)=pthread_create;
static int test_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p) {creates++;if(fail_create&&creates==2)return EAGAIN;return real_create(t,a,f,p);}
#define pthread_create test_create
'''+fn('put_pool_thread')+r'''
static pthread_mutex_t g_put_init=PTHREAD_MUTEX_INITIALIZER;
'''+fn('put_pool_ensure')+fn('put_pool_run')+r'''
static void scenario(unsigned which) {
 struct chunk_put_job jobs[4]={{0}};
 if(which==0) {
  deadline=report_clock_ms();assert(!put_pool_run(jobs,1)&&jobs[0].rc==EFS_ERR_BUSY&&!writes);return;
 }
 if(which==1||which==2) {
  g_put_pool.ready=1;g_put_pool.nworkers=1;g_put_sh[0].count=1;
  pthread_mutex_init(&g_put_sh[0].mu,NULL);pthread_cond_init(&g_put_sh[0].cv,NULL);
  uint64_t start=report_clock_ms();deadline=start+10;
  if(which==2)g_put_pool.shutdown=1;
  assert(!put_pool_run(jobs,4)&&!writes&&g_put_sh[0].count==1);
  for(unsigned i=0;i<4;i++)assert(jobs[i].rc==(which==2?EFS_ERR_IO:EFS_ERR_BUSY)&&!jobs[i].bp);
  assert(report_clock_ms()-start<200);return;
 }
 fail_create=1;assert(!put_pool_run(jobs,4)&&writes==4&&!g_put_pool.ready);
 assert(!put_pool_ensure());int actual=g_put_pool.nworkers;g_put_pool.nworkers=1;
 assert(!put_pool_run(jobs,4)&&writes==8&&g_put_pool.ready&&!deadline);
 g_put_pool.nworkers=actual;
 for(unsigned i=0;i<4;i++)assert(!jobs[i].rc);
 __atomic_store_n(&g_put_pool.shutdown,1,__ATOMIC_RELEASE);
 for(int i=0;i<g_put_pool.nworkers;i++) {pthread_mutex_lock(&g_put_sh[i].mu);pthread_cond_broadcast(&g_put_sh[i].cv);pthread_mutex_unlock(&g_put_sh[i].mu);}
 for(int i=0;i<g_put_pool.nworkers;i++)assert(!pthread_join(g_put_pool.tids[i],NULL));
}
int main(void) {
 for(unsigned i=0;i<4;i++) {pid_t p=fork();assert(p>=0);if(!p){scenario(i);_exit(0);}int status;assert(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));}
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-pool-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('PUT pool: expired inline, full queue timeout, shutdown refusal, worker wakeup and startup recovery PASS')
