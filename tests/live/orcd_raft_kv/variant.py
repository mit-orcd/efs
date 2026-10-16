#!/usr/bin/env python3
"""Apply one test-only KV ablation to a fresh exported source tree.

Never run in a live Git checkout. Correctness fixes, including nested-reader
ownership and staged DEL validation, remain present in every variant.
"""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('variant', choices=['default', 'serial', 'noseek', 'stripe1'])
a = p.parse_args()
if (a.source / '.git').exists():
    p.error('Use an isolated git archive export, not a Git checkout')
changes = {
    'serial': ('src/kv/kv_lsm_internal.h',
               'pthread_rwlock_rdlock(&l->view_rw);',
               'pthread_rwlock_wrlock(&l->view_rw);'),
    'stripe1': ('src/kv/kv_seg.c', '#define KV_SEG_CMU_STRIPES 32',
                '#define KV_SEG_CMU_STRIPES 1'),
    'noseek': ('src/kv/kv_seg.c',
               '!iter_load_cached(it, &it->s->idx[it->bi], seek, plen) && !it->seq)',
               '!iter_load_cached(it, &it->s->idx[it->bi], seek, plen) && !it->seq && 0)'),
}
if a.variant in changes:
    name, before, after = changes[a.variant]
    path = a.source / name
    source = path.read_text()
    if source.count(before) != 1:
        raise RuntimeError('Unexpected source: ' + name)
    path.write_text(source.replace(before, after))
print(a.variant)
