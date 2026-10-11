#!/usr/bin/env python3
"""Actual append admission retains batching after slow/failed publication."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/client/efs_fuse.c').read_text()
start=source.index('static int append_publish_burst(')
body=source[start:source.index('\n}',start)+2]
if os.environ.get('EFS_APPEND_BURST_OLD_TIMING')=='1':
    body=body.replace('r->append_burst_ms=completed_ms;','')
code=r'''
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <stddef.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
typedef uint64_t efs_ino_t;
struct efs_open_ref {efs_ino_t ino;unsigned append_publishing,append_burst;
 uint64_t append_burst_ms,append_handoff_ms;struct efs_open_ref *next;};
static struct efs_open_ref entry={.ino=42},*g_open_refs=&entry;
static pthread_mutex_t g_open_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t clock_ms=100,delay;static unsigned flushes;static int result;
static uint64_t stats_now_ms(void){return clock_ms;}
static int efs_append_flush_report(void *fi,efs_ino_t ino){
 assert(fi==NULL && ino==42 && entry.append_publishing);flushes++;clock_ms+=delay;return result;}
'''+body+r'''
static void reset(void){entry=(struct efs_open_ref){.ino=42};clock_ms=100;delay=0;flushes=0;result=0;}
int main(void){
 reset();delay=1200;
 for(unsigned i=0;i<32;i++)assert(append_publish_burst(42)==0 && !flushes);
 assert(append_publish_burst(42)==0 && flushes==1);
 assert(clock_ms==1300 && entry.append_handoff_ms==1400 && entry.append_burst==1);
 assert(append_publish_burst(42)==0 && flushes==1 && entry.append_burst==2);
 for(unsigned i=0;i<30;i++)assert(append_publish_burst(42)==0 && flushes==1);
 assert(entry.append_burst==32);
 assert(append_publish_burst(42)==0 && flushes==2);
 reset();assert(!append_publish_burst(42));clock_ms+=1000;delay=1500;
 assert(!append_publish_burst(42) && flushes==1 && clock_ms==2600);
 assert(!append_publish_burst(42) && flushes==1);
 clock_ms+=999;assert(!append_publish_burst(42) && flushes==1);
 clock_ms++;assert(!append_publish_burst(42) && flushes==2);
 reset();for(unsigned i=0;i<32;i++)assert(!append_publish_burst(42));result=EFS_ERR_BUSY;
 assert(append_publish_burst(42)==EFS_ERR_BUSY && flushes==1 && entry.append_burst==32 && !entry.append_publishing);
 assert(append_publish_burst(42)==EFS_ERR_BUSY && flushes==2 && entry.append_burst==32);
 result=0;delay=1200;assert(!append_publish_burst(42) && flushes==3 && entry.append_burst==1);
 assert(!append_publish_burst(42) && flushes==3);
 reset();entry.append_publishing=1;assert(!append_publish_burst(42) && !flushes);
 entry.append_publishing=0;assert(!append_publish_burst(99) && !flushes);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-burst-clock-') as temp:
    path=Path(temp)/'test.c';path.write_text(code)
    subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror',str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True,timeout=5)
print('append admission: slow publication preserves 32-write/one-second batching; failed drain must retry PASS')
