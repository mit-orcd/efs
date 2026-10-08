#!/usr/bin/env python3
"""Read-only inventory of a private storage engine fixture."""
import json
from pathlib import Path
import struct
import sys
root=Path(sys.argv[1]);rows={'data_records':0,'logical_bytes':0,'record_allocated':0,'container_allocated':0}
for p in (root/'data').rglob('*'):
    if not p.is_file():continue
    parts=p.relative_to(root/'data').parts
    if len(parts)>=7 and parts[0]=='exports':
        try:
            ino=0
            for s in parts[2:7]:ino=ino*10000+int(s)
            if 0x8000000000000002<=ino<0x8000000000000002+4096:continue
        except ValueError:pass
    try:
        # GC can unlink between traversal, stat and read. Use one metadata
        # snapshot; a later sweep verifies convergence again.
        st=p.stat()
        raw=p.read_bytes() if st.st_size==4096 else b''
    except FileNotFoundError:continue
    size=st.st_size
    rows['logical_bytes']+=struct.unpack('>I',raw[44:48])[0] if raw.startswith(b'EFSCMAP1') else (size-32 if size%4096==32 else size-4096)
    rows['data_records']+=1;rows['record_allocated']+=st.st_blocks*512
for p in (root/'.efs-containers').glob('*.dat'):
    try:rows['container_allocated']+=p.stat().st_blocks*512
    except FileNotFoundError:pass
print(json.dumps(rows))
