#!/usr/bin/env python3
"""Production FUSE locks preserve same-inode exclusion without shard-wide locks."""
from pathlib import Path
import os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/client/efs_fuse.c').read_text()
sections = []
ops = Path(os.environ.get('EFS_DIR_LOCK_SOURCE', root / 'src/client/ops.c')).read_text()
a = ops.index('static int dir_stripe(')
sections.append('#define EFS_DIR_LOCKS 64\n' + ops[a:ops.index('\n}', a)+2])
for start, end in [('#define OPEN_EDGE_SHARDS', '/* Inodes this client'),
                   ('#define EFS_WB_INO_STRIPES', '\nstatic struct {'),
                   ('#define APPEND_MU_N', '/* Close: flush')]:
    a = source.index(start)
    sections.append(source[a:source.index(end, a)])
code = r'''
#include "efs/common.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
''' + '\n'.join(sections) + r'''
static pthread_mutex_t start_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv=PTHREAD_COND_INITIALIZER;
static unsigned ready,go,counter;
static efs_ino_t shared;
static void *worker(void *unused){
 (void)unused;pthread_mutex_lock(&start_mu);ready++;pthread_cond_broadcast(&start_cv);
 while(!go)pthread_cond_wait(&start_cv,&start_mu);pthread_mutex_unlock(&start_mu);
 /* Race the first lookup/initialization, then contend on one real inode. */
 for(unsigned i=0;i<1000;i++){
  pthread_mutex_t *m=efs_wb_ino_lock(shared);pthread_mutex_lock(m);counter++;pthread_mutex_unlock(m);
 }
 return NULL;
}
int main(void){
 shared=((uint64_t)100<<EFS_KV_SHARD_BITS)|17;
 pthread_t threads[16];for(unsigned i=0;i<16;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
 pthread_mutex_lock(&start_mu);while(ready!=16)pthread_cond_wait(&start_cv,&start_mu);
 go=1;pthread_cond_broadcast(&start_cv);pthread_mutex_unlock(&start_mu);
 for(unsigned i=0;i<16;i++)assert(!pthread_join(threads[i],NULL));assert(counter==16000);
 /* Exercise the remaining production directory-lock mapping. All local
  * files differ in allocation counter but retain the same shard bits. */
 for(unsigned shard=0;shard<4096;shard+=17){
  unsigned char seen[EFS_DIR_LOCKS]={0};unsigned count=0;
  for(unsigned i=1;i<=256;i++){
   efs_ino_t ino=((uint64_t)i<<EFS_KV_SHARD_BITS)|shard;
   int slot=dir_stripe(ino);assert(slot>=0&&slot<EFS_DIR_LOCKS&&slot==dir_stripe(ino));
   if(!seen[slot]){seen[slot]=1;count++;}
  }
  assert(count>=48);
 }
 pthread_mutex_t *(*tables[])(efs_ino_t)={open_edge_mu,efs_wb_ino_lock,append_mu};
 for(unsigned table=0;table<3;table++)for(unsigned shard=0;shard<4096;shard+=17){
  pthread_mutex_t *distinct[64];unsigned count=0;
  for(unsigned i=1;i<=256;i++){
   efs_ino_t ino=((uint64_t)i<<EFS_KV_SHARD_BITS)|shard;
   pthread_mutex_t *m=tables[table](ino);assert(m==tables[table](ino));
   unsigned j;for(j=0;j<count && distinct[j]!=m;j++){}
   if(j==count){assert(count<64);distinct[count++]=m;}
  }
  /* Original low-bit modulo sent all 256 same-shard files to one mutex. */
  assert(count>=48);
  assert(!pthread_mutex_lock(distinct[0]));
  assert(pthread_mutex_trylock(distinct[0])==EBUSY);
  assert(!pthread_mutex_trylock(distinct[1]));pthread_mutex_unlock(distinct[1]);
  pthread_mutex_unlock(distinct[0]);
 }
 puts("inode locks: same-shard files spread across the directory and three FUSE production tables; stable same-file exclusion and concurrent initialization PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-inode-stripes-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    flags = ['-fsanitize=address,undefined'] if os.getenv('EFS_TEST_SANITIZE') == '1' else []
    subprocess.run(['cc', *flags, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-pthread', '-I'+str(root/'include'),
                    str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
