#!/usr/bin/env python3
"""Exercise actual credit/checkpoint order, retained queued work and EIO guards."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
 a=s.index('static '+('void' if name=='host_inbox_retire' else 'int')+' '+name+'(')
 return s[a:s.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include "efs/common.h"
#include "efs/raft_flow.h"
#define HOST_NGROUPS 2
#define BO_COUNT(key) ((void)0)
struct efs_raft_host {
 pthread_mutex_t inbox_mu;unsigned inbox_work_n;uint64_t inbox_work_bytes;
 int inbox_pressure,flow_enabled,flow_early_retire,fail,early_seen;
 unsigned writes;uint64_t saved[2];
};
static int persist_applied(struct efs_raft_host*h,int gi,uint64_t idx,uint64_t now,int force) {
 (void)now;(void)force;h->writes++;
 if(!gi) {
  h->early_seen=h->inbox_work_n==16;
  assert(h->early_seen==h->flow_early_retire);
  /* New arrivals during the marker tail retain their own credits. */
  pthread_mutex_lock(&h->inbox_mu);h->inbox_work_n+=8;h->inbox_work_bytes+=1024;pthread_mutex_unlock(&h->inbox_mu);
 }
 if(h->fail==gi+1)return EFS_ERR_IO;
 h->saved[gi]=idx;return EFS_OK;
}
'''+function('host_inbox_retire')+function('host_save_applied')+function('host_checkpoint_retire')+r'''
int main(void) {
 for(int early=0;early<2;early++)for(int failure=0;failure<4;failure++) {
  struct efs_raft_host h={0};uint64_t idx[2]={11,22};
  assert(!pthread_mutex_init(&h.inbox_mu,NULL));h.inbox_work_n=80;h.inbox_work_bytes=6ull<<20;
  h.inbox_pressure=1;h.flow_enabled=1;h.flow_early_retire=early;h.saved[0]=10;h.saved[1]=20;
  h.fail=failure==2?1:failure==3?2:0;
  int rc=host_checkpoint_retire(&h,idx,100,failure==1?EFS_ERR_IO:EFS_OK,64,4ull<<20);
  assert(rc==(failure?EFS_ERR_IO:EFS_OK));
  if(failure==1){assert(!h.writes&&h.saved[0]==10&&h.saved[1]==20);assert(h.inbox_work_n==16);}
  else {
   assert(h.inbox_work_n==24);assert(h.inbox_work_bytes==(2ull<<20)+1024);
   if(failure==2)assert(h.writes==1&&h.saved[0]==10&&h.saved[1]==20);
   if(failure==3)assert(h.writes==2&&h.saved[0]==11&&h.saved[1]==20);
   if(!failure)assert(h.writes==2&&h.saved[0]==11&&h.saved[1]==22);
  }
  assert(!h.inbox_pressure);pthread_mutex_destroy(&h.inbox_mu);
 }
 /* No flow or no processed work must not consume queued credits. */
 for(int early=0;early<2;early++)for(int flow=0;flow<2;flow++) {
  struct efs_raft_host h={0};uint64_t idx[2]={11,22};
  pthread_mutex_init(&h.inbox_mu,NULL);h.flow_early_retire=early;h.flow_enabled=flow;
  h.inbox_work_n=80;h.inbox_work_bytes=6ull<<20;h.inbox_pressure=1;
  assert(host_checkpoint_retire(&h,idx,100,EFS_ERR_IO,flow?0:64,4ull<<20)==EFS_ERR_IO);
  assert(h.inbox_work_n==80&&h.inbox_work_bytes==(6ull<<20)&&h.inbox_pressure==1);
  pthread_mutex_destroy(&h.inbox_mu);
 }
 return 0;
}
'''
# The actual normal pump invokes retirement only after KV release; shutdown
# still forces its marker through the original successful-KV prerequisite.
pump=s[s.index('static void *host_pump('):s.index('static uint64_t make_boot_id(')]
assert pump.index('int kv_rc = efs_kv_lsm_sync_release(h->kv);')<pump.index('host_checkpoint_retire(h,')
assert 'host_persist_durable_tail(h, idx, now_us_(), 1)' in pump
with tempfile.TemporaryDirectory(prefix='efs-host-retire-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('host retirement: credits after KV release/before marker; queued arrivals retained; KV/marker EIO guards PASS')
