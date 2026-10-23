#!/usr/bin/env python3
"""Every live harness and script that hand-builds a CLIENT_HELLO declares
EFS_PROTOCOL_VERSION and expects it back.

Oct 23: K13 raised the protocol to 5 and updated the declarations, but eight
harnesses still asserted a reply of (0, 4); the expiry fixture on the intel
cluster failed with "declaration refused". Fast `make test` group.
"""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
ver = int(re.search(r'#define EFS_PROTOCOL_VERSION (\d+)u', (root/'include/efs/protocol.h').read_text()).group(1))
bad, seen = [], 0
for p in sorted(list((root/'tests/live').glob('*.py')) + list((root/'scripts').glob('*'))):
    if not p.is_file(): continue
    t = p.read_text(errors='replace')
    for m in re.finditer(r"b'EFSP'\+struct\.pack\('=IQ',(\d+),0\)", t):
        seen += 1
        if int(m.group(1)) != ver: bad.append(f'{p.relative_to(root)}: declares {m.group(1)}')
    for m in re.finditer(r"struct\.unpack\('=iI',[a-z_]+\[[0-9:]+\]\)==\(0,(\d+)\)", t):
        seen += 1
        if int(m.group(1)) != ver: bad.append(f'{p.relative_to(root)}: expects reply version {m.group(1)}')
assert seen, 'no hand-built declarations found; update the patterns'
assert not bad, f'protocol {ver}: ' + '; '.join(bad)
print(f'live harness declarations: {seen} sites on protocol {ver} PASS')
