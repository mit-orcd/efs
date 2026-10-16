#!/usr/bin/env python3
"""Exercise actual rollup accounting/admission at synthetic budget boundaries."""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
source = r'''
#include <assert.h>
#include <stdio.h>
#include "metadata.c"
#define SLOT_BYTES (sizeof(efs_ino_t) + sizeof(int64_t) + sizeof(uint8_t))
#define FIRST_BYTES (64 * SLOT_BYTES)
/* Change only the accounted slab payload to place the live table at an exact
 * boundary; no claim that the test allocates this synthetic payload. */
static void at_bytes(struct efs_export *e, uint64_t bytes) {
    uint64_t current = efs_export_staged_bytes(e);
    assert(bytes >= current);
    e->staged_slab_bytes += bytes - current;
    staged_refresh(e);
    assert(efs_export_staged_bytes(e) == bytes);
}
static void accounting(int shard) {
    struct efs_export e = {0}, owner = {0};
    efs_export_init(&e, 1, "budget");
    if (shard) {
        e.owner = &owner;
        owner.staged_total = e.staged_est;
    }
    assert(efs_export_create_with_ino(&e, 16, EFS_ROOT_INO,
                                    S_IFREG | 0644, 0, 0, "file") == 16);
    uint64_t before = efs_export_staged_bytes(&e);
    assert(efs_export_set_size_norollup(&e, 16, 10) == EFS_OK);
    assert(e.pending_rollup_cap == 64);
    assert(efs_export_staged_bytes(&e) == before + FIRST_BYTES);
    efs_export_ensure_rollups(&e);
    assert(e.pending_rollup_count == 0);
    assert(efs_export_staged_bytes(&e) == before + FIRST_BYTES);
    assert(efs_export_set_mtime_ns_norollup(&e, 16, 100, 2) == EFS_OK);
    assert(e.pending_rollup_count == 1);
    assert(efs_export_staged_bytes(&e) == before + FIRST_BYTES);
    efs_export_compact(&e);
    assert(e.pending_rollup_count == 1 && e.pending_rollup_cap == 64);
    efs_export_ensure_rollups(&e);
    efs_export_compact(&e);
    assert(e.pending_rollup_cap == 0);
    assert(!e.pending_rollup_inos && !e.pending_rollup_deltas && !e.pending_rollup_touch);
    assert(efs_export_staged_bytes(&e) == before);
    efs_export_free(&e);
    if (shard) assert(owner.staged_total == 0);
}
static void boundary(unsigned count, int short_by_one, int shard) {
    struct efs_export e = {0}, owner = {0};
    efs_export_init(&e, 1, "boundary");
    for (unsigned i = 0; i < 65; ++i) {
        char name[32]; snprintf(name, sizeof(name), "f%u", i);
        assert(efs_export_create_with_ino(&e, 16 + i, EFS_ROOT_INO,
                                        S_IFREG | 0644, 0, 0, name) == 16 + i);
    }
    if (shard) { e.owner = &owner; owner.staged_total = e.staged_est; }
    for (unsigned i = 0; i < count; ++i)
        assert(efs_export_set_size_norollup(&e, 16 + i, 1) == EFS_OK);
    uint64_t allocation = (count ? 128 : 64) * SLOT_BYTES;
    uint64_t target = efs_stage_cap_bytes() - allocation + short_by_one;
    at_bytes(&e, target);
    uint64_t old_cap = e.pending_rollup_cap;
    assert(efs_export_set_size_norollup(&e, 16 + count, 1) == EFS_OK);
    if (short_by_one) {
        assert(e.pending_rollup_cap == old_cap);
        assert(e.pending_rollup_count == 0);
        assert(efs_export_staged_bytes(&e) == target);
    } else {
        assert(e.pending_rollup_cap == (count ? 128 : 64));
        assert(e.pending_rollup_count == count + 1);
        assert(efs_export_staged_bytes(&e) == target + allocation - old_cap * SLOT_BYTES);
    }
    efs_export_ensure_rollups(&e);
    struct efs_inode row;
    assert(efs_export_get_inode(&e, EFS_ROOT_INO, &row) == EFS_OK);
    assert(row.tree_bytes == count + 1);
    assert(row.imm_files == 65);
    /* Above-target pinned storage may remain; another optional queue
     * allocation must not compound it. */
    efs_export_compact(&e);
    assert(e.pending_rollup_cap == 0);
    at_bytes(&e, efs_stage_cap_bytes() + 1);
    assert(efs_export_set_size_norollup(&e, 80, 3) == EFS_OK);
    assert(e.pending_rollup_cap == 0 && e.pending_rollup_count == 0);
    efs_export_ensure_rollups(&e);
    assert(efs_export_get_inode(&e, EFS_ROOT_INO, &row) == EFS_OK);
    assert(row.tree_bytes == count + 4 - (count == 64));
    efs_export_free(&e);
    if (shard) assert(owner.staged_total == 0);
}
int main(void) {
    assert(setenv("EFS_CLIENT_META_MB", "16", 1) == 0);
    for (int shard = 0; shard <= 1; ++shard) {
        accounting(shard);
        for (int short_by_one = 0; short_by_one <= 1; ++short_by_one) {
            boundary(0, short_by_one, shard);
            boundary(64, short_by_one, shard);
        }
    }
    puts("rollup budget: retained capacity, root/shard totals, exact/short boundaries, eager fallback and compaction PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rollup-budget-') as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c99', '-D_GNU_SOURCE', '-g', '-pthread',
                    '-DBLAKE3_USE_NEON=0', '-DBLAKE3_NO_SSE2', '-DBLAKE3_NO_SSE41',
                    '-DBLAKE3_NO_AVX2', '-DBLAKE3_NO_AVX512',
                    *(['-fsanitize=address,undefined'] if args.sanitize else []),
                    '-I' + str(root / 'include'), '-I' + str(root / 'src/meta'),
                    '-I' + str(root / 'deps/blake3'), str(c),
                    *[str(root / name) for name in ('src/common/common.c',
                        'src/common/checksum.c', 'src/data/erasure.c',
                        'deps/blake3/blake3.c', 'deps/blake3/blake3_portable.c',
                        'deps/blake3/blake3_dispatch.c')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
