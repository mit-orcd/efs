#!/usr/bin/env python3
"""Production STALE replay fan-out stops claiming work once its budget expires."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M);return s[m.start():s.index('\n}',m.end())+2]
def block(name):
 a=s.index('struct '+name+' {');return s[a:s.index('\n};',a)+3]
code=r'''
#include "efs/common.h"
#include <pthread.h>
#include <assert.h>
#define REPLAY_THREADS 16
static __thread uint64_t deadline;
static unsigned calls;static int expired,expire_first;
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
static int efs_client_rpc_past_deadline(void) {return __atomic_load_n(&expired,__ATOMIC_RELAXED);}
static int dcache_replay_stale_ex(efs_ino_t i,uint32_t ci,int absent) {(void)i;(void)ci;(void)absent;assert(deadline==12345);__sync_fetch_and_add(&calls,1);if(expire_first)__atomic_store_n(&expired,1,__ATOMIC_RELAXED);return EFS_OK;}
'''+block('stale_pair')+block('replay_fan')+fn('replay_fan_thread')+fn('replay_fan_run')+r'''
int main(void) {
 struct stale_pair p[64]={{0}};deadline=12345;assert(!replay_fan_run(p,64)&&calls==64&&deadline==12345);
 expired=1;assert(replay_fan_run(p,64)==EFS_ERR_BUSY&&calls==64);
 struct replay_fan f={.pp=p,.n=64,.deadline=12345,.mu=PTHREAD_MUTEX_INITIALIZER};
 deadline=999;replay_fan_thread(&f);assert(deadline==999&&!f.next&&f.error==EFS_ERR_BUSY);
 expired=0;expire_first=1;calls=0;f.error=0;
 replay_fan_thread(&f);assert(calls==1&&f.next==1&&f.error==EFS_ERR_BUSY&&deadline==999);
 f.error=EFS_ERR_IO;replay_fan_thread(&f);assert(f.error==EFS_ERR_IO&&calls==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-replay-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('STALE replay fanout: inherited deadline, expired admission, mid-run stop and prior-error retention PASS')
