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

# Raft command and request tags share one byte on the wire: no two may collide
# (Oct 23: K13-D31 PUT_ORPHAN_ACK and K29 TXN_BATCH both took 86 in parallel).
meta = (Path(__file__).resolve().parents[1] / 'include/efs/meta_cmd.h').read_text()
tags = {}
for name, val in re.findall(r'^#define (EFS_MD_(?:CMD|REQ)_[A-Z0-9_]+)\s+(\d+)\b', meta, flags=re.M):
    v = int(val)
    assert v not in tags, f'Raft tag {v} used by both {tags[v]} and {name}'
    tags[v] = name
assert all(0 < v < 256 for v in tags), 'a Raft tag must fit one byte'
print(f'Raft command/request tags: {len(tags)} distinct PASS')

# Wire message types (enum in protocol.h): no two may share a value
# (Oct 23: K13-D31 PUT_ORPHAN_DELETE and D39 FORMATION_READ both took 177/178).
proto = (Path(__file__).resolve().parents[1] / 'include/efs/protocol.h').read_text()
msgs = {}
for name, val in re.findall(r'^\s*(EFS_MSG_[A-Z0-9_]+)\s*=\s*(\d+)\s*,', proto, flags=re.M):
    v = int(val)
    assert v not in msgs, f'message type {v} used by both {msgs[v]} and {name}'
    msgs[v] = name
assert msgs and all(0 < v < 256 for v in msgs), 'message types must fit one byte'
print(f'wire message types: {len(msgs)} distinct PASS')

