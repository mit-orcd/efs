#!/usr/bin/env python3
"""W140 oracle regression: legitimate background entries may advance indexes."""
from pathlib import Path
import struct
import tempfile
import zlib
from raft_log_entries import assert_command_absent, decode_entries

HEADER=struct.pack('>II',0x52464C31,1)
def entry(index,tag):
    command=bytes([tag])+bytes(8)
    body=struct.pack('>BIQQI',2,0,index,1,len(command))+command
    return struct.pack('>II',zlib.crc32(body),len(body))+body

log=HEADER+entry(2,18)+entry(3,78)+bytes(32)
assert decode_entries(log)==[(0,2,1,18),(0,3,1,78)]
with tempfile.TemporaryDirectory(prefix='efs-refusal-oracle-') as d:
    path=Path(d)/'raft.log';path.write_bytes(log)
    assert assert_command_absent([path],200)==2
    path.write_bytes(HEADER+entry(2,18)+entry(3,200))
    try:assert_command_absent([path],200)
    except AssertionError:pass
    else:raise AssertionError('oracle missed an appended refused command')
for bad in (log[:-33],HEADER+entry(2,18)[:-1],HEADER+entry(2,18)+b'x',
            HEADER+entry(2,18)[:9]+b'\xff'+entry(2,18)[10:]):
    try:decode_entries(bad)
    except ValueError:pass
    else:raise AssertionError('malformed evidence accepted')
print('PASS refusal oracle: background PACK_INDEX allowed, refused tag detected, malformed evidence refused')
