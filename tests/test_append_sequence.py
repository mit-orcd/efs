#!/usr/bin/env python3
"""Actual append identity allocator is unique across per-inode lock domains."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text();a=s.index('static uint64_t append_next_sequence(');f=s[a:s.index('\n}',a)+2]
if os.environ.get('APPEND_TEST_OLD'):
 f='static uint64_t append_next_sequence(void) {uint64_t seq=++g_client.append_opid_seq;if(!seq)seq=++g_client.append_opid_seq;return seq;}'
code=r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#define N 8
#define M 100000
static struct {uint64_t append_opid_seq;} g_client;
static uint64_t values[N][M];
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
'''+f+r'''
static void *worker(void *arg) {
 unsigned i=(unsigned)(uintptr_t)arg;
 pthread_mutex_t inode_lock=PTHREAD_MUTEX_INITIALIZER;
 pthread_mutex_lock(&gate);pthread_mutex_unlock(&gate);
 for(unsigned j=0;j<M;j++) {
  pthread_mutex_lock(&inode_lock);values[i][j]=append_next_sequence();pthread_mutex_unlock(&inode_lock);
 }
 pthread_mutex_destroy(&inode_lock);return NULL;
}
static int cmp(const void *a,const void *b) {uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);}
int main(void) {
 pthread_t t[N];pthread_mutex_lock(&gate);
 for(unsigned i=0;i<N;i++)assert(!pthread_create(&t[i],NULL,worker,(void *)(uintptr_t)i));
 pthread_mutex_unlock(&gate);
 for(unsigned i=0;i<N;i++)assert(!pthread_join(t[i],NULL));
 qsort(values,N*M,sizeof(uint64_t),cmp);
 for(unsigned i=0;i<N*M;i++)assert(((uint64_t *)values)[i]==i+1);
 g_client.append_opid_seq=UINT64_MAX-1;assert(append_next_sequence()==UINT64_MAX);
 assert(!append_next_sequence()&&g_client.append_opid_seq==UINT64_MAX);
 assert(!append_next_sequence()&&g_client.append_opid_seq==UINT64_MAX);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-seq-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O2','-std=gnu11','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('append sequence: 800000 concurrent unique IDs and fail-closed exhaustion PASS')
