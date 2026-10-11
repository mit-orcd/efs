#!/usr/bin/env python3
"""Fault the actual rollup queue growth; no FUSE or cluster required."""
from pathlib import Path
import argparse
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'src/meta/metadata.c')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
text = args.source.read_text()
match = re.search(r'^static int pending_rollup_note\([^;]+?\)\n\{', text, re.M)
if not match:
    raise RuntimeError('pending_rollup_note definition missing')
function = text[match.start():text.index('\n}', match.end()) + 2]
source = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "efs/metadata.h"
static unsigned allocation_call, failure;
static void *fault_malloc(size_t n) {
    if (++allocation_call == failure) return NULL;
    return malloc(n);
}
/* Also supports running the regression against the original realloc code.
 * Every successful resize moves; old allocations here contain 64 entries. */
static void *fault_realloc(void *old, size_t n) {
    void *next = fault_malloc(n);
    if (!next) return NULL;
    if (old) { memcpy(next, old, n / 2); free(old); }
    return next;
}
#define malloc fault_malloc
#define realloc fault_realloc
''' + function + r'''
#undef malloc
#undef realloc
static void release(struct efs_export *e) {
    free(e->pending_rollup_inos);
    free(e->pending_rollup_deltas);
    free(e->pending_rollup_touch);
}
static void contents(struct efs_export *e, unsigned count) {
    assert(e->pending_rollup_count == count);
    for (unsigned i = 0; i < count; ++i) {
        assert(e->pending_rollup_inos[i] == i + 1);
        assert(e->pending_rollup_deltas[i] == (int64_t)i - 32);
        assert(e->pending_rollup_touch[i] == (i & 1));
    }
}
static void scenario(unsigned count, unsigned fail, int cleanup_only) {
    struct efs_export e = {0};
    failure = 0;
    for (unsigned i = 0; i < count; ++i)
        assert(pending_rollup_note(&e, i + 1, (int64_t)i - 32, i & 1) == 0);
    contents(&e, count);
    efs_ino_t *old_inos = e.pending_rollup_inos;
    int64_t *old_deltas = e.pending_rollup_deltas;
    uint8_t *old_touch = e.pending_rollup_touch;
    uint64_t old_cap = e.pending_rollup_cap;
    allocation_call = 0;
    failure = fail;
    assert(pending_rollup_note(&e, 999, -7, 1) == -1);
    assert(e.pending_rollup_cap == old_cap);
    assert(e.pending_rollup_inos == old_inos);
    assert(e.pending_rollup_deltas == old_deltas);
    assert(e.pending_rollup_touch == old_touch);
    contents(&e, count);
    /* Both ordinary owner cleanup and a retry must be safe after failure. */
    if (cleanup_only) { release(&e); return; }
    failure = 0;
    assert(pending_rollup_note(&e, 999, -7, 1) == 0);
    assert(e.pending_rollup_count == count + 1);
    assert(e.pending_rollup_inos[count] == 999);
    assert(e.pending_rollup_deltas[count] == -7);
    assert(e.pending_rollup_touch[count] == 1);
    assert(pending_rollup_note(&e, 999, 4, 0) == 0);
    assert(e.pending_rollup_count == count + 1);
    assert(e.pending_rollup_deltas[count] == -3);
    assert(e.pending_rollup_touch[count] == 1);
    e.pending_rollup_count = 0;
    allocation_call = 0; failure = 1;
    assert(pending_rollup_note(&e, 1234, 5, 0) == 0);
    assert(allocation_call == 0);
    assert(e.pending_rollup_inos[0] == 1234);
    release(&e);
}
int main(void) {
    (void)fault_realloc;
    for (unsigned count = 0; count <= 64; count += 64)
        for (unsigned fail = 1; fail <= 3; ++fail)
            for (int cleanup = 0; cleanup <= 1; ++cleanup)
                scenario(count, fail, cleanup);
    puts("rollup allocation failures: initial/growth, cleanup/retry and coalescing pass");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rollup-oom-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(source)
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c99', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-g',
                    '-I' + str(root / 'include'), *flags, str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
