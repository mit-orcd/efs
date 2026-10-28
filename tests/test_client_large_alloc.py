#!/usr/bin/env python3
"""Actual startup allocator policy returns large transient payload pages.

The old four-arena-only policy is the negative control. A large freed
transfer first raises glibc's adaptive threshold; later half-MiB transient
buffers interleaved with small live anchors must not become heap holes.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_ALLOCATOR_SOURCE',root/'src/client/efs_fuse.c')).read_text()
m=re.search(r'^static int fuse_allocator_init\(void\)\n\{',s,re.M)
policy=s[m.start():s.index('\n}',m.end())+2]
code=r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __GLIBC__
#include <malloc.h>
'''+policy+r'''
#define N 24
#define BLOCKS 8
#define BYTES (512*1024)
static pthread_barrier_t full,release,freed,finish;
static volatile unsigned char *visible;
static void *worker(void *arg){
 unsigned char *data[BLOCKS],*anchor[BLOCKS];unsigned char id=(unsigned char)(long)arg;
 for(unsigned i=0;i<BLOCKS;i++){
  data[i]=malloc(BYTES);anchor[i]=malloc(64);if(!data[i]||!anchor[i])abort();
  memset(data[i],id,BYTES);memset(anchor[i],id,64);
 }
 pthread_barrier_wait(&full);pthread_barrier_wait(&release);
 for(unsigned i=0;i<BLOCKS;i++){
  for(unsigned j=0;j<BYTES;j++)if(data[i][j]!=id)abort();
  free(data[i]);
 }
 pthread_barrier_wait(&freed);pthread_barrier_wait(&finish);
 for(unsigned i=0;i<BLOCKS;i++){for(unsigned j=0;j<64;j++)if(anchor[i][j]!=id)abort();free(anchor[i]);}
 return NULL;
}
int main(int argc,char **argv){
 (void)argv;
 if(argc==1){if(fuse_allocator_init())return 2;}
 else if(!mallopt(M_ARENA_MAX,4))return 2;
 /* This allocation escapes optimization and, in the old policy, raises
  * the dynamic threshold when freed, just like a completed large reply. */
 unsigned char *p=malloc(8*1024*1024);if(!p)return 3;
 memset(p,37,8*1024*1024);visible=p;
 if(visible[8191]!=37)return 3;
 free(p);visible=NULL;
 pthread_t threads[N];pthread_barrier_init(&full,NULL,N+1);pthread_barrier_init(&release,NULL,N+1);
 pthread_barrier_init(&freed,NULL,N+1);pthread_barrier_init(&finish,NULL,N+1);
 for(long i=0;i<N;i++)if(pthread_create(&threads[i],NULL,worker,(void *)(i+1)))return 4;
 pthread_barrier_wait(&full);struct mallinfo2 live=mallinfo2();
 pthread_barrier_wait(&release);pthread_barrier_wait(&freed);struct mallinfo2 after=mallinfo2();
 int ok=live.hblkhd>=64*1024*1024 && after.hblkhd<live.hblkhd/4 && after.arena<16*1024*1024;
 printf("workers=%d payloads=exact live_mmap=%zu released_mmap=%zu retained_heap=%zu free_heap=%zu\n",N,live.hblkhd,after.hblkhd,after.arena,after.fordblks);
 pthread_barrier_wait(&finish);for(unsigned i=0;i<N;i++)pthread_join(threads[i],NULL);
 return ok?0:1;
}
#else
int main(void){puts("large allocation: NOTRUN non-glibc allocator");return 3;}
#endif
'''
with tempfile.TemporaryDirectory(prefix='efs-large-alloc-') as tmp:
 p=Path(tmp);(p/'case.c').write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra','-Werror','-pthread',str(p/'case.c'),'-o',str(p/'case')],check=True)
 env={k:v for k,v in os.environ.items() if not k.startswith('MALLOC_')}
 fixed=subprocess.run([str(p/'case')],env=env,capture_output=True,text=True,timeout=30)
 if fixed.returncode==3 and 'NOTRUN non-glibc' in fixed.stdout:
  print(fixed.stdout.strip());raise SystemExit(0)
 assert fixed.returncode==0,fixed.stdout+fixed.stderr
 negative=subprocess.run([str(p/'case'),'four-arenas-only'],env=env,capture_output=True,text=True,timeout=30)
 assert negative.returncode==1,negative.stdout+negative.stderr
 print('fixed:',fixed.stdout.strip());print('four-arena-only negative:',negative.stdout.strip())
print('client large allocations: pages returned despite live small anchors, old adaptive policy refused PASS')
