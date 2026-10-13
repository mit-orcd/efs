#!/usr/bin/env python3
"""Production marker worker: one slot, durable-prefix ownership and drain."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('struct host_checkpoint_worker {');b=s.index('struct host_kv_worker {',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#define HOST_NGROUPS 2
#define HOST_APPLIED_MAX_TAIL 1024ull
#define BO_ON() 0
#define BH_APPLIED_PERSIST_US 0
#define BO_HIST(k,n) ((void)(k),(void)(n))
static uint64_t efs_batch_obs_now_us(void){return 0;}
struct efs_raft_host {int flow_enabled;struct {uint64_t applied_saved;} g[2];};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int paused,entered,create_fail,result_rc;
static unsigned markers,credits;static uint64_t last_idx,last_bytes;
static int efsd_pthread_create(pthread_t *tid,void *(*fn)(void*),void *arg){return create_fail?1:pthread_create(tid,NULL,fn,arg);}
static int host_checkpoint_retire(struct efs_raft_host *h,const uint64_t *idx,
 uint64_t now,int kv_rc,unsigned n,uint64_t bytes){(void)h;assert(now);
 pthread_mutex_lock(&gate);
 if(kv_rc){credits+=n;last_bytes=bytes;pthread_mutex_unlock(&gate);return kv_rc;}
 entered++;pthread_cond_broadcast(&changed);
 while(paused)pthread_cond_wait(&changed,&gate);
 int rc=result_rc;if(!rc){markers++;last_idx=idx[0];for(unsigned i=0;i<2;i++)h->g[i].applied_saved=idx[i];}credits+=n;last_bytes=bytes;
 pthread_mutex_unlock(&gate);return rc;
}
'''+s[a:b]+r'''
struct call {struct host_checkpoint_worker *w;int stop,rc,done;uint64_t prefix;unsigned n;};
static void *invoke(void *arg){struct call *c=arg;uint64_t idx[2]={c->prefix,c->prefix+1};
 c->rc=c->stop?host_checkpoint_stop(c->w):host_checkpoint_dispatch(c->w,idx,2,0,c->n,200);
 __atomic_store_n(&c->done,1,__ATOMIC_RELEASE);return NULL;}
static void wait_entered(unsigned n){pthread_mutex_lock(&gate);while((unsigned)entered<n)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);}
static void release(void){pthread_mutex_lock(&gate);paused=0;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}
static void idle(struct host_checkpoint_worker *w){pthread_mutex_lock(&w->mu);while(w->busy)pthread_cond_wait(&w->cv,&w->mu);pthread_mutex_unlock(&w->mu);}
int main(void){
 struct efs_raft_host h={.flow_enabled=1};struct host_checkpoint_worker w;uint64_t idx[2]={10,11};
 create_fail=1;assert(host_checkpoint_start(&w,&h)==EFS_ERR_IO && !w.started);assert(!host_checkpoint_stop(&w));create_fail=0;
 assert(!host_checkpoint_start(&w,&h));paused=1;
 assert(!host_checkpoint_dispatch(&w,idx,1,0,1,100));idx[0]=999;wait_entered(1);
 /* A failed later KV barrier cannot overwrite/enqueue the captured job. */
 assert(host_checkpoint_dispatch(&w,idx,2,EFS_ERR_IO,3,300)==EFS_ERR_IO);
 assert(w.busy && w.idx[0]==10 && !markers && credits==3);
 uint64_t same[2]={10,11};
 /* The accepted prefix requires no second slot, including when its I/O is
  * blocked. No pending credits may disappear through this shortcut. */
 assert(!host_checkpoint_dispatch(&w,same,2,0,0,0) && w.busy && w.idx[0]==10);
 h.flow_enabled=0;
 assert(!host_checkpoint_dispatch(&w,same,2,0,5,500) && w.busy && credits==3);
 h.flow_enabled=1;
 uint64_t later[2]={1022,1023};
 assert(!host_checkpoint_dispatch(&w,later,2,0,0,0) && w.idx[0]==10);
 /* The cap uses completed markers (still zero), not the accepted prefix. */
 struct call c={.w=&w,.prefix=1023,.n=0};pthread_t cap;
 assert(!pthread_create(&cap,NULL,invoke,&c));usleep(10000);
 assert(!__atomic_load_n(&c.done,__ATOMIC_ACQUIRE));release();
 pthread_join(cap,NULL);assert(!c.rc);idle(&w);
 assert(w.saved[0]==1023 && w.saved[1]==1024);
 /* Reset for the credit/backpressure test. */
 assert(!host_checkpoint_stop(&w));assert(!host_checkpoint_start(&w,&h));
 entered=markers=credits=0;paused=1;idx[0]=10;
 assert(!host_checkpoint_dispatch(&w,idx,1,0,1,100));wait_entered(1);
 c=(struct call){.w=&w,.prefix=20,.n=2};pthread_t tid;assert(!pthread_create(&tid,NULL,invoke,&c));
 usleep(10000);assert(!__atomic_load_n(&c.done,__ATOMIC_ACQUIRE));
 pthread_mutex_lock(&w.mu);assert(w.busy && w.idx[0]==10);pthread_mutex_unlock(&w.mu);
 release();pthread_join(tid,NULL);assert(!c.rc);idle(&w);
 assert(markers==2 && last_idx==20 && credits==3 && last_bytes==200);
 same[0]=20;same[1]=21;
 assert(!host_checkpoint_dispatch(&w,same,2,0,0,0));idle(&w);assert(markers==2);
 assert(!host_checkpoint_dispatch(&w,same,2,0,1,10));idle(&w);assert(markers==3 && credits==4);
 /* Errors are delivered to the pump; a newer successful prefix can repair. */
 pthread_mutex_lock(&gate);result_rc=EFS_ERR_IO;pthread_mutex_unlock(&gate);
 assert(!host_checkpoint_dispatch(&w,idx,3,0,1,1));idle(&w);
 pthread_mutex_lock(&gate);result_rc=0;pthread_mutex_unlock(&gate);
 assert(host_checkpoint_dispatch(&w,idx,4,0,1,1)==EFS_ERR_IO);idle(&w);
 assert(!host_checkpoint_stop(&w));
 /* Stop waits for the accepted job, even when its marker barrier is blocked. */
 entered=markers=credits=0;paused=1;assert(!host_checkpoint_start(&w,&h));idx[0]=30;
 assert(!host_checkpoint_dispatch(&w,idx,5,0,1,100));wait_entered(1);
 c=(struct call){.w=&w,.stop=1};assert(!pthread_create(&tid,NULL,invoke,&c));
 usleep(10000);assert(!__atomic_load_n(&c.done,__ATOMIC_ACQUIRE));
 release();pthread_join(tid,NULL);assert(!c.rc && !w.started && markers==1 && last_idx==30 && credits==1);
 assert(!host_checkpoint_start(&w,&h));result_rc=EFS_ERR_IO;
 assert(!host_checkpoint_dispatch(&w,idx,6,0,0,0));assert(host_checkpoint_stop(&w)==EFS_ERR_IO);
 puts("checkpoint worker: one captured slot, KV failure refusal, backpressure, error repair, creation fallback and shutdown drain PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-checkpoint-worker-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run(['cc',*flags,'-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
a=s.index('static void *host_pump(');b=s.index('\n}',a);pump=s[a:b]
assert pump.index('efs_kv_lsm_sync_release(h->kv)')<pump.index('host_checkpoint_dispatch(')
assert pump.index('host_checkpoint_stop(')<pump.index('host_persist_durable_tail(')
