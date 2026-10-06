#!/usr/bin/env python3
"""Discard only phantom span-only report marks, retaining real local ownership."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = (root / 'src/client/write.c').read_text()
def function(name):
    m = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    return text[m.start():text.index('\n}', m.end()) + 2]
source = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
struct dcache_ent { int dirty, stalled, pin_held; uint64_t object_gen, committed_object, object_seq, committed_seq; };
static struct dcache_ent entry, *current;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int marks;
static void dcache_ensure(void) {}
static uint32_t dcache_slot(efs_ino_t ino, uint32_t ci) { (void)ino; (void)ci; return 0; }
static pthread_mutex_t *dcache_mu(uint32_t slot) { (void)slot; return &mu; }
static struct dcache_ent *dcache_find_meta(uint32_t s, efs_ino_t ino, uint32_t ci) { (void)s; (void)ino; (void)ci; return current; }
void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t ci) { (void)ino; (void)ci; assert(!pthread_mutex_trylock(&mu)); pthread_mutex_unlock(&mu); ++marks; }
''' + '\n'.join(function(n) for n in ['dcache_keep_on_drop', 'dcache_pending_of', 'report_retry_without_identity']) + r'''
int main(void) {
    report_retry_without_identity(1, 0); assert(!marks);
    current = &entry;
    entry.object_gen = entry.committed_object = 9;
    entry.object_seq = entry.committed_seq = 7;
    report_retry_without_identity(1, 0); assert(!marks);
    entry.dirty = 1; report_retry_without_identity(1, 0); assert(marks == 1);
    entry.dirty = 0; entry.stalled = 1; report_retry_without_identity(1, 0); assert(marks == 2);
    entry.stalled = 0; entry.pin_held = 1; report_retry_without_identity(1, 0); assert(marks == 3);
    entry.pin_held = 0; entry.object_seq = 8; report_retry_without_identity(1, 0); assert(marks == 4);
    entry.committed_seq = 8; report_retry_without_identity(1, 0); assert(marks == 4);
    puts("span-only report marks: phantom cleared, dirty/stalled/pinned/unreported ownership retained PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-orphan-') as d:
    p = Path(d) / 'test.c'; p.write_text(source)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'src/client'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
