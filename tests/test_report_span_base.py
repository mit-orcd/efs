#!/usr/bin/env python3
"""An owned span must not borrow the CAS base of a newer dirty overwrite.

Executes production PUT-record capture/readers and REPORT ownership selection.
Allocation/table lookup are adapters. SPAN_READER_SOURCE selects old production
readers for a compile-and-execute negative control, not a compile failure.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
current = (root/'src/client/write.c').read_text()
readers = Path(os.environ['SPAN_READER_SOURCE']).read_text() if os.getenv('SPAN_READER_SOURCE') else current


def fn(text, name):
    m = re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{', text, re.M)
    assert m, name
    return text[m.start():text.index('\n}', m.end())+2]


def struct(name):
    start = current.index('struct '+name+' {')
    return current[start:current.index('\n};', start)+3]


code = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DCACHE_NR 32
#define PUTID_TOMB UINT64_MAX
''' + struct('put_id') + '\n' + struct('dcache_ent') + r'''
static struct put_id owned;
static struct dcache_ent slot;
static unsigned have_owned, missed;
static uint64_t g_putid_live;
static pthread_mutex_t g_putid_mu=PTHREAD_MUTEX_INITIALIZER,mu=PTHREAD_MUTEX_INITIALIZER;
static struct put_id *putid_find(efs_ino_t ino,uint32_t ci) {
 return have_owned && owned.ino==ino && owned.ci==ci ? &owned : NULL;
}
static struct put_id *putid_insert_slot(efs_ino_t ino,uint32_t ci) {
 (void)ino;(void)ci;have_owned=1;return &owned;
}
static void dcache_ensure(void) {}
static uint32_t dcache_slot(efs_ino_t ino,uint32_t ci) {(void)ino;(void)ci;return 0;}
static pthread_mutex_t *dcache_mu(uint32_t i) {(void)i;return &mu;}
static struct dcache_ent *dcache_find_meta(uint32_t i,efs_ino_t ino,uint32_t ci) {
 (void)i;return slot.ino==ino && slot.ci==ci ? &slot : NULL;
}
static void putid_miss_note(efs_ino_t ino,uint32_t ci,const efs_node_id_t *nodes) {
 (void)ino;(void)ci;(void)nodes;missed++;
}
static void report_retry_without_identity(efs_ino_t ino,uint32_t ci) {(void)ino;(void)ci;}
''' + '\n'.join([fn(current,'putid_note'), fn(readers,'putid_fill'),
    fn(current,'dcache_unpublished'), fn(readers,'dcache_object_of'),
    fn(current,'report_fill_owned')]) + r'''
static efs_node_id_t nodes[3]={1,2,3};
static uint8_t checksums[3][EFS_HASH_SIZE];
static void capture(uint64_t object,uint64_t sequence,uint32_t length,uint64_t base) {
 putid_note(42,0,object,sequence,nodes,checksums,0,length,0,0,NULL,base);
}
static struct efs_chunk_rec report(uint64_t newer_base) {
 struct efs_chunk_rec rec={.ino=42,.base_gen=newer_base};uint64_t seq=0;
 assert(report_fill_owned(42,0,&rec,&seq,nodes));
 assert(rec.nodes[0]==1 && rec.nodes[1]==2 && rec.nodes[2]==3);
 if(rec.delta_len)assert(rec.base_gen!=EFS_CHUNK_BASE_UNCOND);
 return rec;
}
int main(void) {
 /* PUT of a ten-byte span on G7 finishes. Before REPORT, a later full
  * overwrite changes the dirty slot to UNCOND. Only the old object is owned
  * by this report snapshot: newer dirty bytes are not its publication. */
 capture(11,3,10,7);
 struct efs_chunk_rec rec=report(EFS_CHUNK_BASE_UNCOND);
 assert(rec.chunk_generation==11 && rec.delta_len==10 && rec.base_gen==7);
 rec=report(99);assert(rec.base_gen==7);
 /* Late completion cannot replace a newer retained PUT contract. */
 capture(88,2,10,2);rec=report(EFS_CHUNK_BASE_UNCOND);
 assert(rec.chunk_generation==11 && rec.base_gen==7);
 capture(12,4,10,13);rec=report(EFS_CHUNK_BASE_UNCOND);
 assert(rec.chunk_generation==12 && rec.base_gen==13);
 /* Ordinary full-image base progression stays with its existing owner. */
 capture(14,5,0,0);rec=report(EFS_CHUNK_BASE_UNCOND);
 assert(!rec.delta_len && rec.base_gen==EFS_CHUNK_BASE_UNCOND);
 have_owned=0;
 slot.ino=42;slot.base_gen=EFS_CHUNK_BASE_UNCOND;
 slot.object_gen=11;slot.object_seq=3;slot.object_span_base_gen=7;
 slot.object_delta_len=10;
 memcpy(slot.object_nodes,nodes,sizeof(nodes));
 rec=report(slot.base_gen);assert(rec.chunk_generation==11 && rec.base_gen==7);
 slot.object_span_base_gen=0;rec=report(slot.base_gen);assert(!rec.base_gen);
 slot.committed_object=11;slot.committed_seq=3;
 uint64_t seq=0;assert(!report_fill_owned(42,0,&rec,&seq,nodes) && missed==1);
 puts("REPORT span base: retained PUT/fallback survive newer UNCOND image, old completion excluded, empty-base exact, full-image behavior preserved PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-span-') as d:
    p = Path(d)/'test.c'; p.write_text(code)
    subprocess.run(shlex.split(os.getenv('CC','cc'))+['-std=gnu11','-g','-O1',
        '-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),
        '-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
