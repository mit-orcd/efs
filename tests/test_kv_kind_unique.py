#!/usr/bin/env python3
"""Every EFS_KV_KIND_* number is distinct (outside the reserved range markers).

Oct 23: W154 (DEGRADED) and K14 (PUB_STAGE) were written in parallel and both
took kind 51; the merge compiled and every test passed, while degraded marks
and staged publications would have shared one key space on disk. Run in the
fast `make test` group.
"""
from pathlib import Path
import re
src = (Path(__file__).resolve().parents[1] / 'include/efs/kv_key.h').read_text()
kinds = re.findall(r'^#define (EFS_KV_KIND_[A-Z0-9_]+)\s+(\d+)\b', src, flags=re.M)
lo = hi = None
seen = {}
for name, val in kinds:
    v = int(val)
    if name == 'EFS_KV_KIND_RESERVED_MIN': lo = v; continue
    if name == 'EFS_KV_KIND_RESERVED_MAX': hi = v; continue
    assert v not in seen, f'KV kind {v} used by both {seen[v]} and {name}'
    seen[v] = name
assert lo is not None and hi is not None, 'reserved range markers missing'
inside = [n for v, n in seen.items() if lo <= v <= hi]
assert not inside, f'kinds inside the reserved range {lo}..{hi}: {inside}'
assert all(0 < v < 256 for v in seen), 'a KV kind must fit the one-byte key field'
print(f'KV kinds: {len(seen)} distinct, none in reserved {lo}..{hi} PASS')
