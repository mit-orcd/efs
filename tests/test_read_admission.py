#!/usr/bin/env python3
"""Exercise the production demand scratch recovery boundary without cache locks."""
from pathlib import Path
import subprocess, tempfile, os, shlex
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/read.c').read_text()
a=s.index('static void *demand_read_alloc(')
f=s[a:s.index('\n}',a)+2]
source=r'''
#include <assert.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
static unsigned calls,trimmed,waits,succeed;
static unsigned expire_after;
static int efs_client_rpc_past_deadline(void){return expire_after&&waits>=expire_after-1;}
static int body;
static void *efs_buf_alloc(uint32_t n) {assert(n==131072);calls++;return calls==succeed?&body:NULL;}
static void efs_rdcache_trim(void) {trimmed++;}
static int test_sleep(const struct timespec *t, struct timespec *r) {
 (void)r;assert(t->tv_sec==0 && t->tv_nsec==1000000);waits++;return 0;
}
#define nanosleep test_sleep
''' + f + r'''
static void reset(unsigned n){calls=trimmed=waits=expire_after=0;succeed=n;}
int main(void) {
 reset(1);assert(demand_read_alloc(131072)==&body && !trimmed && !waits);
 reset(2);assert(demand_read_alloc(131072)==&body && trimmed==1 && !waits);
 reset(5);assert(demand_read_alloc(131072)==&body && trimmed==1 && waits==3);
 reset(0);assert(!demand_read_alloc(131072) && calls==21 && trimmed==1 && waits==20);
 reset(0);expire_after=1;assert(!demand_read_alloc(131072)&&!calls&&!trimmed&&!waits);
 reset(0);expire_after=4;assert(!demand_read_alloc(131072)&&calls==4&&trimmed==1&&waits==3);
 puts("demand read: fast path, reclaim, delayed release and bounded exhaustion PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-admission-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 exe=Path(d)/'test'
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
# Both queued prefetch and reproducible demand cache copies use the watermark.
assert 'job->chunk = efs_buf_alloc_prefetch(cs);' in s
assert 'e->data = efs_buf_alloc_prefetch(len);' in s
assert 'jobs[batch].chunk = demand_read_alloc(chunk_size);' in s
