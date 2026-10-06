#!/usr/bin/env python3
"""Actual GETCHUNKS fan-out must carry the caller's recovery deadline."""
import re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/ops.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
a=s.index('struct pull_fan {');b=s.index('\n};',a)+3
code='''#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include "efs/common.h"
#define PULL_FAN 16
static __thread uint64_t deadline;
static unsigned calls;
static uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
static void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline=d; }
struct pull_absent;
static int pull_chunks_range(efs_ino_t ino,uint32_t start,uint32_t end,struct pull_absent *ab) {
(void)ino;(void)start;(void)end;(void)ab;
assert(deadline==12345);__sync_fetch_and_add(&calls,1);return EFS_OK;
}
'''+s[a:b]+fn('pull_fan_thread')+fn('pull_groups_parallel')+'''
int main(void) {
deadline=12345;assert(!pull_groups_parallel(1,0,64*64,NULL));
assert(calls==64 && deadline==12345);
struct pull_fan f={.ino=1,.next=0,.end=64,.deadline=12345,.mu=PTHREAD_MUTEX_INITIALIZER};
deadline=999;pull_fan_thread(&f);assert(deadline==999);
return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-pull-deadline-') as d:
 p=Path(d)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(['cc','-std=gnu11','-pthread','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('GETCHUNKS fan-out: inherited deadline and caller TLS restoration PASS')
