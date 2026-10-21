#!/usr/bin/env python3
"""Exercise the production demand scratch recovery boundary without cache locks.

Demand reads allocate from the read headroom (efs_buf_alloc_read), trim the
read cache once, then wait for transient buffers (1 ms steps, 10 ms after
100) until the caller's deadline, or 30 s without one: pending writes
holding the budget must not fail a read after 20 ms (orcd-mini big test).
"""
from pathlib import Path
import subprocess, tempfile, os, shlex
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/read.c').read_text()
a=s.index('static void *demand_read_alloc(uint32_t len)\n{')
f=s[a:s.index('\n}',a)+2]
source=r'''
#include <assert.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
static unsigned calls,trimmed,waits,succeed;
static uint64_t now_ms, deadline;       /* fake monotonic clock and caller deadline */
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static int efs_client_rpc_past_deadline(void){return deadline && now_ms>=deadline;}
static int body;
static void *efs_buf_alloc_read(uint32_t n) {assert(n==131072);calls++;return calls==succeed?&body:NULL;}
static void efs_rdcache_trim(void) {trimmed++;}
static int test_sleep(const struct timespec *t, struct timespec *r) {
 (void)r;assert(t->tv_sec==0 && (t->tv_nsec==1000000 || t->tv_nsec==10000000));
 now_ms+=t->tv_nsec/1000000;waits++;return 0;
}
static int test_clock(clockid_t c, struct timespec *ts) {
 (void)c;ts->tv_sec=(time_t)(now_ms/1000);ts->tv_nsec=(long)(now_ms%1000)*1000000;return 0;
}
#define nanosleep test_sleep
#define clock_gettime test_clock
''' + f + r'''
static void reset(unsigned n,uint64_t dl){calls=trimmed=waits=0;succeed=n;now_ms=1000000;deadline=dl?now_ms+dl:0;}
int main(void) {
 reset(1,30000);assert(demand_read_alloc(131072)==&body && !trimmed && !waits);
 reset(2,30000);assert(demand_read_alloc(131072)==&body && trimmed==1 && !waits);
 reset(5,30000);assert(demand_read_alloc(131072)==&body && trimmed==1 && waits==3);
 /* long holders: waits beyond the old 20 ms, 10 ms steps after 100 */
 reset(400,30000);assert(demand_read_alloc(131072)==&body && waits==398 && now_ms==1000000+100+298*10);
 /* the caller's deadline ends the wait */
 reset(0,500);assert(!demand_read_alloc(131072) && now_ms>=1000500 && now_ms<1000600 && trimmed==1);
 reset(0,1);now_ms=deadline;assert(!demand_read_alloc(131072)&&!calls&&!trimmed&&!waits);
 /* no deadline: still bounded, 30 s */
 reset(0,0);assert(!demand_read_alloc(131072) && now_ms>1000000+30000 && now_ms<1000000+32000);
 puts("demand read: fast path, reclaim, waits for transient buffers within the deadline, bounded without one PASS");
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
assert 'void *scratch = demand_read_alloc((uint32_t)needed);' in s, 'packed-read scratch must wait like demand reads'
