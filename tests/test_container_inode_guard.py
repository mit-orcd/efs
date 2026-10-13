#!/usr/bin/env python3
"""Exact container inode guards exclude PUT during death without shard collapse."""
import os,shlex,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_STORE_SOURCE',root/'src/server/store.c')).read_text()
a=s.index('static pthread_rwlock_t *inode_gc_lock(');b=s.index('#define DEATH_PAGE_BYTES',a)
code=r'''
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#define FRAG_LOCKS 4096


struct efsd_server {int storage_engine;};
static pthread_rwlock_t inode_gc_locks[FRAG_LOCKS];
static pthread_once_t frag_locks_once=PTHREAD_ONCE_INIT;
static void frag_locks_init(void){for(int i=0;i<FRAG_LOCKS;i++)pthread_rwlock_init(&inode_gc_locks[i],NULL);}
'''+s[a:b]+r'''
int main(void){
 struct efsd_server files={0},containers={1};unsigned seen[FRAG_LOCKS]={0};
 for(uint64_t n=1;n<=1024;n++){
  uint64_t ino=(n<<EFS_KV_SHARD_BITS)|7;
  pthread_rwlock_t *p=server_inode_gc_lock(&containers,1,ino);
  assert(!seen[p-inode_gc_locks]);seen[p-inode_gc_locks]=1;
  assert(server_inode_gc_lock(&files,1,ino)==inode_gc_lock(1,ino));
  assert(server_inode_gc_lock(NULL,1,ino)==inode_gc_lock(1,ino));
 }
 uint64_t a=(100ull<<12)|7,b=(101ull<<12)|7;
 pthread_rwlock_t *ga=server_inode_gc_lock(&containers,1,a),*gb=server_inode_gc_lock(&containers,1,b);
 assert(ga!=gb);assert(!pthread_rwlock_wrlock(ga));
 assert(pthread_rwlock_tryrdlock(server_inode_gc_lock(&containers,1,a))!=0);
 assert(!pthread_rwlock_trywrlock(gb));pthread_rwlock_unlock(gb);pthread_rwlock_unlock(ga);
 assert(!pthread_rwlock_rdlock(ga));assert(pthread_rwlock_trywrlock(ga)!=0);pthread_rwlock_unlock(ga);
 assert(!pthread_rwlock_trywrlock(ga));pthread_rwlock_unlock(ga);
 puts("container inode guards: 1024 same-shard ordinals dispersed, exact-inode read/write exclusion, files unchanged PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-inode-guard-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
# No active storage path may keep the old hash independently of its peer.
assert s[s.index('#define DEATH_PAGE_BYTES'):].count('inode_gc_lock(')==5
assert s[s.index('#define DEATH_PAGE_BYTES'):].count('server_inode_gc_lock(')==5
