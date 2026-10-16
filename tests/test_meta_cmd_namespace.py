#!/usr/bin/env python3
"""Reject ambiguous command tags before independent branch protocols are merged."""
import re
from pathlib import Path

def validate(text):
    found = {}
    for name, literal in re.findall(r'^#define\s+(EFS_MD_(?:CMD|REQ)_\w+)\s+(\S+)', text, re.M):
        assert re.fullmatch(r'[0-9]+', literal), (name, 'explicit numeric tag required', literal)
        tag = int(literal)
        assert 1 <= tag <= 255, (name, 'tag outside one-byte command namespace', tag)
        assert tag not in found, ('command tag collision', tag, found.get(tag), name)
        found[tag] = name
    assert found, 'no command tags checked'
    return found

header = (Path(__file__).resolve().parents[1] / 'include/efs/meta_cmd.h').read_text()
current = validate(header)
# Retired feature tags stay reserved: no live tag may land inside 45-51/55-57.
bounds = {name: tag for tag, name in current.items() if 'RESERVED' in name}
assert bounds == {'EFS_MD_CMD_RESERVED_MIN': 45, 'EFS_MD_CMD_RESERVED_MAX': 51,
                  'EFS_MD_CMD_RESERVED2_MIN': 55, 'EFS_MD_CMD_RESERVED2_MAX': 57}, bounds
for tag, name in current.items():
    assert 'RESERVED' in name or not (45 <= tag <= 51 or 55 <= tag <= 57), ('reserved tag reused', tag, name)
# Reserved commands may coexist with this completion command.
assert current[52] == 'EFS_MD_CMD_APPEND_CATCHUP'
coexist = header if 45 in current else header + '\n#define EFS_MD_CMD_RESERVED_MIN 45\n'
validate(coexist)
for bad in (coexist.replace('EFS_MD_CMD_APPEND_CATCHUP 52', 'EFS_MD_CMD_APPEND_CATCHUP 45'),
            header + '\n#define EFS_MD_CMD_BAD_WIDTH 256\n'):
    try:
        validate(bad)
    except AssertionError:
        pass
    else:
        raise AssertionError('invalid namespace accepted')
print('metadata command namespace: actual header unique, 45-51/55-57 stay reserved; reserved/completion collision and byte overflow rejected PASS')
