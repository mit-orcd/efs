#!/usr/bin/env python3
"""Exercise production GET admission and worker lifetime with real tiny queues."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/read.c').read_text()
def fn(p):
 a=s.index(p);return s[a:s.index('\n}',a)+2]
def block(n):
 a=s.index('struct '+n+' {');return s[a:s.index('\n};',a)+3]
a=s.index('#define GET_POOL_QDEPTH');b=s.index('static void *get_pool_thread(',a)
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static __thread uint64_t deadline;
static unsigned reads,frees;
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_rpc_past_deadline(void){return deadline&&now_ms()>=deadline;}
int efs_net_remaining_ms(int cap){if(!deadline)return cap;uint64_t now=now_ms();if(now>=deadline)return 0;return deadline-now<(uint64_t)cap?(int)(deadline-now):cap;}
static uint32_t data_chunk_size(void){return 131072;}
void efs_buf_free(void*p,uint32_t n){(void)n;__sync_fetch_and_add(&frees,1);free(p);}
'''+block('get_batch')+block('chunk_get_job')+r'''
static void *chunk_get_worker(void*p){struct chunk_get_job*j=p;assert(deadline&&!efs_client_rpc_past_deadline());__sync_fetch_and_add(&reads,1);usleep(1000);j->rc=EFS_OK;return NULL;}
'''+fn('static int read_retry_pause(')+fn('static uint64_t get_budget_deadline(')+fn('static int get_wait_budget(')+fn('static void *get_job_run(')+'\n'+s[a:b].replace('#define GET_POOL_QDEPTH 32','#define GET_POOL_QDEPTH 1').replace('#define GET_POOL_N 64','#define GET_POOL_N 2')+fn('static void *get_pool_thread(')+fn('static int get_pool_ensure(void)')+fn('void efs_client_read_pools_stop(void)')+fn('static int get_pool_run_budgeted(')+fn('static int get_pool_run(')+r'''
#define RDCACHE_STRIPES 1
#define RDCACHE_WAYS 2
struct rdcache_ent {efs_ino_t ino;uint32_t ci;int pending;uint8_t*data;uint64_t gen,tick;uint32_t len;};
static struct {struct rdcache_ent e[1][2];pthread_mutex_t mu;pthread_cond_t cv[1];uint64_t tick;} g_rdcache={.mu=PTHREAD_MUTEX_INITIALIZER,.cv={PTHREAD_COND_INITIALIZER}};
static uint64_t rdcache_map_gen(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;return 1;}
static uint32_t rdcache_slot(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;return 0;}
static pthread_mutex_t*rdcache_mu(uint32_t slot){(void)slot;return &g_rdcache.mu;}
'''+'static int rdcache_acquire(efs_ino_t ino, uint32_t ci, uint8_t *dst,\n                           uint32_t len)\n{\n    uint64_t tg = rdcache_map_gen(ino, ci);\n    uint32_t s = rdcache_slot(ino, ci);\n    pthread_mutex_t *mu = rdcache_mu(s);\n    int stripe = (int)(s & (RDCACHE_STRIPES - 1));\n\n    if (!dst || !len)\n        return 1;\n    for (;;) {\n        struct rdcache_ent *e = NULL;\n        struct rdcache_ent *pend = NULL;\n        int w;\n\n        pthread_mutex_lock(mu);\n        for (w = 0; w < RDCACHE_WAYS; w++) {\n            struct rdcache_ent *c = &g_rdcache.e[s][w];\n            if (c->ino == ino && c->ci == ci) {\n                if (c->pending)\n                    pend = c;\n                else if (c->data)\n                    e = c;\n            }\n        }\n        if (e && e->gen == tg && e->len >= len && tg) {\n            memcpy(dst, e->data, len);\n            e->tick = ++g_rdcache.tick;\n            pthread_mutex_unlock(mu);\n            return 0;\n        }\n        if (pend) {\n            int rc = get_wait_budget(&g_rdcache.cv[stripe], mu);\n            pthread_mutex_unlock(mu);\n            if (rc != EFS_OK)\n                return rc;\n            continue;\n        }\n        pthread_mutex_unlock(mu); return 1;\n    }\n}\n'+r'''
static void scenario(unsigned which){
 struct chunk_get_job jobs[4]={{0}};
 if(which==0){deadline=now_ms();uint64_t old=deadline;assert(!get_pool_run(jobs,1)&&jobs[0].rc==EFS_ERR_BUSY&&!reads&&deadline==old);return;}
 if(which==1||which==2){
  g_get_pool.ready=1;g_get_pool.nworkers=1;g_get_sh[0].count=1;
  pthread_mutex_init(&g_get_sh[0].mu,NULL);pthread_cond_init(&g_get_sh[0].cv,NULL);
  uint64_t start=now_ms();deadline=start+10;uint64_t old=deadline;
  if(which==2)g_get_pool.shutdown=1;
  assert(!get_pool_run(jobs,4)&&!reads&&g_get_sh[0].count==1&&deadline==old);
  for(unsigned i=0;i<4;i++)assert(jobs[i].rc==(which==2?EFS_ERR_IO:EFS_ERR_BUSY)&&!jobs[i].bp);
  assert(now_ms()-start<200);return;
 }
 if(which==3){
  assert(!get_pool_ensure());int actual=g_get_pool.nworkers;g_get_pool.nworkers=1;
  assert(!get_pool_run(jobs,4)&&reads==4&&!deadline);
  for(unsigned i=0;i<4;i++)assert(!jobs[i].rc&&!jobs[i].bp);
  g_get_pool.nworkers=actual;efs_client_read_pools_stop();return;
 }
 if(which==4){
  pthread_mutex_init(&g_get_sh[0].mu,NULL);pthread_cond_init(&g_get_sh[0].cv,NULL);
  struct chunk_get_job*j=calloc(1,sizeof(*j));j->owned=1;j->chunk=malloc(data_chunk_size());j->deadline=now_ms();
  g_get_sh[0].q[0]=j;g_get_sh[0].count=1;g_get_pool.shutdown=1;
  get_pool_thread(NULL);assert(frees==1&&!reads&&!deadline);return;
 }
 if(which==5){
  g_rdcache.e[0][0].ino=42;g_rdcache.e[0][0].ci=3;g_rdcache.e[0][0].pending=1;
  uint8_t dst[8];uint64_t start=now_ms();deadline=start+10;
  assert(rdcache_acquire(42,3,dst,sizeof(dst))==EFS_ERR_BUSY&&now_ms()-start<200);
  assert(g_rdcache.e[0][0].pending&&g_rdcache.e[0][0].ino==42);
  assert(!pthread_mutex_trylock(&g_rdcache.mu));pthread_mutex_unlock(&g_rdcache.mu);return;
 }
 uint64_t start=now_ms();deadline=start+10;assert(read_retry_pause(100000)==EFS_ERR_BUSY&&now_ms()-start<200);
 assert(read_retry_pause(50000)==EFS_ERR_BUSY);deadline=0;assert(read_retry_pause(1000)==EFS_OK);
}
int main(void){for(unsigned i=0;i<7;i++){pid_t p=fork();assert(p>=0);if(!p){scenario(i);_exit(0);}int st;assert(waitpid(p,&st,0)==p&&WIFEXITED(st)&&!WEXITSTATUS(st));}return 0;}
'''
# Keep queue declaration and ring arithmetic equally small.
code=code.replace('#define GET_POOL_QDEPTH 128','#define GET_POOL_QDEPTH 1')
with tempfile.TemporaryDirectory(prefix='efs-get-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('GET pool: expired inline, queue timeout, shutdown, immediate wakeup, prefetch cleanup, pending cache ownership and capped retry sleeps PASS')
