#!/usr/bin/env python3
"""Execute production pressure/chain indexes against a full-scan oracle."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/write.c').read_text()
def fn(name):
 a=s.index(name+'(');a=s.rfind('\n',0,a)+1
 b=s.index('\n}',a)+2
 return s[a:b]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#define DCACHE_SLOTS 65536
#define DCACHE_SHARDS 64
typedef uint64_t efs_ino_t;
struct dcache_ent {efs_ino_t ino;void *data,*writer;uint64_t object_gen,committed_object,object_seq,committed_seq;
int dirty,stalled,pin_held,on_dirty,reclaim_claimed,present_extra;struct dcache_ent *next;};
static struct {pthread_mutex_t shard[DCACHE_SHARDS];struct dcache_ent e[DCACHE_SLOTS];
uint64_t pressure_slots[DCACHE_SHARDS][16],chain_slots[DCACHE_SHARDS][16];} g_dcache;
static void dcache_ensure(void) {}
static int efs_writer_state_owned(void *w) {return w!=NULL;}
static void efs_writer_state_free(void *w) {assert(!w);}
static void efs_buf_metadata_free(void *p,size_t n) {assert(n==sizeof(struct dcache_ent));free(p);}
'''+fn('dcache_index_set')+fn('dcache_metadata_idle')+fn('dcache_keep_on_drop')+fn('efs_dcache_trim_metadata')+fn('efs_dcache_pressure_ino')+r'''
static efs_ino_t oracle(efs_ino_t after) {
efs_ino_t found=0;for(unsigned i=0;i<DCACHE_SLOTS;i++)for(struct dcache_ent *e=&g_dcache.e[i];e;e=e->next)
if(e->ino>after&&(e->data||e->object_gen)&&dcache_keep_on_drop(e)&&(!found||e->ino<found))found=e->ino;
return found;
}
int main(void) {
for(int i=0;i<64;i++)pthread_mutex_init(&g_dcache.shard[i],NULL);
/* Populate collisions and all ownership states. Candidate order must exactly
 * match the old oracle, including clean PUT-complete, stalled and pinned. */
for(unsigned i=0;i<4096;i++) {
unsigned slot=(i*197)%65536;struct dcache_ent *e=&g_dcache.e[slot];
e->ino=1+(i*83)%1009;e->data=(void*)1;e->dirty=(i%5)==0;e->stalled=(i%5)==1;
e->pin_held=(i%5)==2;e->object_gen=7;e->object_seq=2;
e->committed_object=7;e->committed_seq=(i%5)==3?1:2;
dcache_index_set(g_dcache.pressure_slots,slot);
}
for(unsigned after=0;after<1010;after++)assert(efs_dcache_pressure_ino(after)==oracle(after));
/* Passing the cursor does not erase still-owned candidates. */
assert(!efs_dcache_pressure_ino(UINT64_MAX));assert(efs_dcache_pressure_ino(0)==oracle(0));
for(unsigned i=0;i<65536;i++) {struct dcache_ent *e=&g_dcache.e[i];e->dirty=e->stalled=e->pin_held=0;e->committed_seq=e->object_seq;}
assert(!efs_dcache_pressure_ino(0));
/* A later PUT result must re-arm even when the old body/candidate disappeared. */
struct dcache_ent *e=&g_dcache.e[65535];e->ino=4000;e->data=NULL;e->object_gen=9;e->committed_object=0;
dcache_index_set(g_dcache.pressure_slots,65535);assert(efs_dcache_pressure_ino(0)==4000);
/* Chain cleanup retains unpublished and writer-owned metadata, then reaps only
 * after ownership is resolved. Empty bits can be rearmed on collision reuse. */
struct dcache_ent *a=calloc(1,sizeof(*a)),*b=calloc(1,sizeof(*b));assert(a&&b);
g_dcache.e[77].next=a;a->next=b;b->ino=3;b->object_gen=5;
dcache_index_set(g_dcache.chain_slots,77);efs_dcache_trim_metadata();assert(g_dcache.e[77].next==b);
b->committed_object=5;b->writer=(void*)1;efs_dcache_trim_metadata();assert(g_dcache.e[77].next==b);
b->writer=NULL;efs_dcache_trim_metadata();assert(!g_dcache.e[77].next);
a=calloc(1,sizeof(*a));assert(a);g_dcache.e[77].next=a;dcache_index_set(g_dcache.chain_slots,77);
efs_dcache_trim_metadata();assert(!g_dcache.e[77].next);
puts("pressure index: oracle, cursor, failed/unpublished ownership, late PUT rearm, chain retirement PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pressure-index-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+shlex.split(os.environ.get('EXTRA_CFLAGS',''))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=30)
