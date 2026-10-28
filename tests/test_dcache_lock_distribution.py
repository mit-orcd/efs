#!/usr/bin/env python3
"""Allocated files sharing a metadata shard must not share all cache I/O locks.
Execute the production slot/lock selection, hold one file's I/O mutex and
prove a distinct file can enter while same-key exclusion remains intact.
An optional unchanged write.c is a runtime negative control.
"""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[1]
s=(Path(sys.argv[1]) if len(sys.argv)>1 else root/'src/client/write.c').read_text()
def fn(name):
 a=s.index('static ',s.rfind('\n',0,s.index(name+'(')))
 b=s.index('\n}',a)+2
 return s[a:b]
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#define DCACHE_SLOTS 65536
#define DCACHE_SHARDS 64
static struct {pthread_mutex_t shard_io[DCACHE_SHARDS];} g_dcache;
static void dcache_ensure(void) {}
'''+fn('dcache_slot')+'\n'+fn('dcache_io_mu')+r'''
int main(void) {
 for(unsigned i=0;i<DCACHE_SHARDS;i++)assert(!pthread_mutex_init(&g_dcache.shard_io[i],NULL));
 /* Real allocator stride: the bottom 12 bits are a metadata shard. */
 for(unsigned ci=0;ci<8;ci++) {
  unsigned slots[DCACHE_SLOTS]={0},locks[DCACHE_SHARDS]={0},ns=0,nl=0;
  for(unsigned i=1;i<=64;i++) {
   uint32_t slot=dcache_slot((uint64_t)i*4096+1,ci);
   assert(slot<DCACHE_SLOTS);
   ns+=!slots[slot]++;nl+=!locks[slot%DCACHE_SHARDS]++;
  }
  assert(ns>=56 && nl>=24); /* old: 16 cache buckets, one I/O mutex */
 }
 uint32_t a=dcache_slot(4097,0),b=dcache_slot(8193,0);
 pthread_mutex_t *ma=dcache_io_mu(a),*mb=dcache_io_mu(b);
 assert(ma!=mb);
 assert(!pthread_mutex_lock(ma));
 assert(pthread_mutex_trylock(dcache_io_mu(dcache_slot(4097,0)))==EBUSY);
 assert(!pthread_mutex_trylock(mb)); /* unrelated file is not parked behind a's RPC */
 assert(!pthread_mutex_unlock(mb));assert(!pthread_mutex_unlock(ma));
 for(unsigned i=0;i<DCACHE_SHARDS;i++)assert(!pthread_mutex_destroy(&g_dcache.shard_io[i]));
 puts("dcache allocated-inode I/O independence, chunk distribution and same-key exclusion PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-dcache-lock-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
