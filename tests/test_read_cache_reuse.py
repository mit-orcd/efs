#!/usr/bin/env python3
"""Production mapping equality: unchanged views reuse, every byte identity change evicts."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/client/ops.c').read_text()
m = re.search(r'static int chunk_rec_same_read\([^;]+?\)\n\{', s)
fn = s[m.start():s.index('\n}', m.end())+2]
source = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
''' + fn + r'''
int main(void) {
    struct efs_chunk_entry ce = {0};
    struct efs_chunk_rec r = {0}, changed;
    ce.ino=r.ino=7; ce.chunk_index=r.chunk_index=3;
    ce.generation=r.chunk_generation=91;
    ce.ndelta=r.delta_base_n=2; ce.delta_seq=r.delta_base_seq=4;
    ce.fragment_nodes[0]=r.nodes[0]=1;
    ce.checksums[0][0]=r.checksums[0][0]=17;
    ce.deltas[0].generation=6; ce.deltas[0].len=3;
    ce.deltas[1].generation=9; ce.deltas[1].off=7; ce.deltas[1].len=8;
    memcpy(r.deltas,ce.deltas,sizeof(r.deltas));
    ce.read_view=(struct efs_fence_view){.chunk_size=131072,.count=3,.revision=9};
    ce.read_view.parts[0].len=131072;
    ce.read_view.parts[1].len=3;
    ce.read_view.parts[2].off=7; ce.read_view.parts[2].len=8;
    r.read_view=ce.read_view;
    assert(chunk_rec_same_read(&ce,&r));
#define CHANGE(expr) do { changed=r; expr; assert(!chunk_rec_same_read(&ce,&changed)); } while(0)
    CHANGE(changed.ino++); CHANGE(changed.chunk_index++);
    CHANGE(changed.chunk_generation++); CHANGE(changed.nodes[1]++);
    CHANGE(changed.checksums[2][31]++);
    CHANGE(changed.delta_base_n--); CHANGE(changed.delta_base_seq++);
    CHANGE(changed.deltas[0].generation++); CHANGE(changed.deltas[0].seq++);
    CHANGE(changed.deltas[0].off++); CHANGE(changed.deltas[0].len++);
    CHANGE(changed.deltas[0].nodes[2]++); CHANGE(changed.deltas[0].checksums[1][1]++);
    CHANGE(changed.read_view.fence_epoch++); CHANGE(changed.read_view.revision++);
    CHANGE(changed.read_view.chunk_size*=2); CHANGE(changed.read_view.count--);
    CHANGE(changed.read_view.parts[0].len--); CHANGE(changed.read_view.parts[1].epoch++);
    CHANGE(changed.read_view.parts[2].off++);
    CHANGE(changed.packed.segment_id++); CHANGE(changed.packed.record_generation++);
    CHANGE(changed.packed.digest[0]++);
    struct efs_chunk_entry copy;
    uint64_t key=efs_chunk_read_key(&ce);
#define KEY_CHANGE(expr) do {copy=ce;expr;assert(efs_chunk_read_key(&copy)!=key);} while(0)
    KEY_CHANGE(copy.deltas[0].generation++);
    KEY_CHANGE(copy.deltas[0].seq++); KEY_CHANGE(copy.deltas[0].off++);
    KEY_CHANGE(copy.deltas[0].len++); KEY_CHANGE(copy.deltas[0].nodes[1]++);
    KEY_CHANGE(copy.deltas[0].checksums[1][0]++);
    KEY_CHANGE(copy.fragment_nodes[2]++); KEY_CHANGE(copy.checksums[2][31]++);
    KEY_CHANGE(copy.read_view.parts[1].epoch++);
    KEY_CHANGE(copy.read_view.parts[1].len++);
    changed=r;changed.deltas[7].len++;assert(chunk_rec_same_read(&ce,&changed));
    changed=r;changed.chunk_generation=0;changed.base_gen=91;
    assert(chunk_rec_same_read(&ce,&changed));
    ce.ndelta=changed.delta_base_n=EFS_CHUNK_DELTA_MAX+1;
    assert(!chunk_rec_same_read(&ce,&changed));
    puts("read cache identity: unchanged, placement, checksums, spans, fences, packed identity PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-reuse-') as d:
    p=Path(d)/'test.c'; p.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    cmd+=shlex.split(os.environ.get('READ_REUSE_CFLAGS',''))
    subprocess.run(cmd+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
