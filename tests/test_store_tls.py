#!/usr/bin/env python3
"""Actual aligned-page owner: reuse within a thread, free on retirement."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/store.c').read_text();a=s.index('struct shard_io_buffers {');b=s.index('static void *shard_io_thread(',a)
head='''#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
static unsigned live;
static void *tracked_calloc(size_t n,size_t size) { void *p=calloc(n,size);if(p)__sync_add_and_fetch(&live,1);return p; }
static void tracked_free(void *p) {if(p)__sync_sub_and_fetch(&live,1);free(p);}
#define calloc tracked_calloc
#define free tracked_free
'''
tail='''
#undef calloc
#undef free
static void *worker(void *arg) {
 (void)arg;struct shard_io_buffers *p=shard_io_buffers_get();assert(p);
 assert(p==shard_io_buffers_get());
 assert(!posix_memalign((void **)&p->tail,4096,4096));__sync_add_and_fetch(&live,1);
 assert(!posix_memalign((void **)&p->bounce,4096,69632));__sync_add_and_fetch(&live,1);
 p->bounce_len=69632;assert(p->tail!=p->bounce);return NULL;
}
int main(void) {for(int round=0;round<50;round++){pthread_t threads[8];for(int i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));for(int i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));assert(live==0);}return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-store-tls-') as td:
 p=Path(td)/'test.c';p.write_text(head+s[a:b]+tail);binary=Path(td)/'test'
 subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
print('direct I/O TLS: actual owner reuse, 400 concurrent thread retirements, zero outstanding allocations PASS')
