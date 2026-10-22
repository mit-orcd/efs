#!/usr/bin/env python3
"""A partial load without a base never inherits a slot's UNCOND base.

A slot whose full-chunk body was published keeps base_gen UNCOND after the
body is dropped. A later partial write whose chunk map was evicted loads
zeros + its range with have_base=0; if the slot stayed UNCOND the flush
skipped the merge-base fetch and published zeros + range as the whole chunk
(W125 closure C1, 2026-10-21). dcache_apply_init must reset the base to 0
then, keep UNCOND for a complete image, and set UNCOND for a full write.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = Path(os.environ.get('PARTIAL_UNCOND_SOURCE', root / 'src/client/write.c')).read_text()
def function(name):
    m = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    return text[m.start():text.index('\n}', m.end()) + 2]
a = text.index('struct dcache_ent {'); entry = text[a:text.index('\n};', a) + 3]
a = text.index('struct dcache_init {'); init = text[a:text.index('\n};', a) + 3]
source = r'''
#include "client_internal.h"
#include "efs/writer_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint64_t efs_client_epoch_get(efs_ino_t ino) {(void)ino;return 0;}
void efs_client_epoch_note(efs_ino_t ino,uint64_t e) {(void)ino;(void)e;}
int efs_client_putid_owns(efs_ino_t ino,uint32_t ci,uint64_t g) {(void)ino;(void)ci;(void)g;return 0;}
#define DCACHE_NR 32
#define DTRACE(...) ((void)0)
''' + entry + '\n' + init + '\n' + function('dcache_add_range') + '\n' + function('dcache_apply_init') + r'''
int main(void) {
    struct dcache_ent e;
    /* Evicted map: no base, slot was UNCOND from the first full write. */
    memset(&e, 0, sizeof(e)); e.base_gen = EFS_CHUNK_BASE_UNCOND;
    struct dcache_init in = {0, 0, 0, 1000, 100};
    dcache_apply_init(&e, &in);
    assert(!e.have_base && e.base_gen == 0);
    assert(e.nrange == 1 && e.roff[0] == 1000 && e.rlen[0] == 100);
    /* Complete image (fetched base): keeps UNCOND as before. */
    memset(&e, 0, sizeof(e)); e.base_gen = EFS_CHUNK_BASE_UNCOND;
    in = (struct dcache_init){1, 77, 0, 0, 10};
    dcache_apply_init(&e, &in);
    assert(e.have_base && e.base_gen == EFS_CHUNK_BASE_UNCOND);
    /* Ordinary slot: the fetched base generation. */
    memset(&e, 0, sizeof(e));
    dcache_apply_init(&e, &in);
    assert(e.have_base && e.base_gen == 77);
    /* Full write: UNCOND. */
    memset(&e, 0, sizeof(e));
    in = (struct dcache_init){.full = 1};
    dcache_apply_init(&e, &in);
    assert(e.have_base && e.base_gen == EFS_CHUNK_BASE_UNCOND && e.nrange == 0);
    puts("partial uncond: a baseless partial load resets an UNCOND slot to merge PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-partial-uncond-') as directory:
    path = Path(directory) / 'test.c'; path.write_text(source)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'src/client')]
    subprocess.run(command + [str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
