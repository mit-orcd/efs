#!/usr/bin/env python3
"""A partial overwrite must never publish only its uncovered suffix."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = Path(os.environ.get('SPAN_TEST_SOURCE', root / 'src/client/write.c')).read_text()
m = re.search(r'^static void span_of\([^;]+?\)\n\{', text, re.M)
function = text[m.start():text.index('\n}', m.end()) + 2]
source = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct efs_chunk_entry map;
static uint64_t own;
static int export_chunk_copy(efs_ino_t ino, uint32_t ci, struct efs_chunk_entry *out) {
    (void)ino; (void)ci; *out = map; return EFS_OK;
}
static int putid_fill(efs_ino_t ino, uint32_t ci, struct efs_chunk_rec *out, uint64_t *seq) {
    (void)ino; (void)ci; (void)seq; out->chunk_generation = own; out->delta_len = own ? 4096 : 0; return own != 0;
}
''' + function + r'''
int main(void) {
    uint32_t off = 1024, len = 4096, out, n;
    map.ndelta = 1; map.deltas[0] = (struct efs_chunk_delta){.off = 0, .len = 4096, .generation = 7};
    span_of(1, 1, 0, 1, &off, &len, 131072, &out, &n);
    assert(!out && !n); /* Original code discarded [1024,4096). */
    off = 4096;
    span_of(1, 1, 0, 1, &off, &len, 131072, &out, &n);
    assert(out == off && n == len);
    off = 0; len = 8192;
    span_of(1, 1, 0, 1, &off, &len, 131072, &out, &n);
    assert(!out && !n);
    own = 7;
    span_of(1, 1, 0, 1, &off, &len, 131072, &out, &n);
    assert(out == off && n == len);
    map.ndelta = EFS_CHUNK_DELTA_MAX;
    span_of(1, 1, 0, 1, &off, &len, 131072, &out, &n);
    assert(!out && !n);
    puts("span selection: partial overlap folds, disjoint and local unpublished ranges preserved PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-span-selection-') as directory:
    path = Path(directory) / 'test.c'; path.write_text(source)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'src/client')]
    command += shlex.split(os.environ.get('SPAN_TEST_CFLAGS', ''))
    subprocess.run(command + [str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
