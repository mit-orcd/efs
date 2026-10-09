#!/usr/bin/env python3
"""Bounded display/export gates, with a private fixture and caller-owned files."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from query_rpc import page_request, query
mount, peer, work = map(Path, sys.argv[1:4])
root = mount / ('efs-pages-%d' % os.getpid())
root.mkdir()
for i in range(215):
    (root / ('item-%03d.txt' % i)).write_bytes(b'x')
other = peer / root.name

def command(*args, expected=0):
    p = subprocess.run([sys.executable, str(other / '.efs/find'), '.txt', '--match', 'suffix', *args],
                       capture_output=True, timeout=60)
    assert p.returncode == expected, (args, p.returncode, p.stderr)
    return p

end = time.monotonic() + 30
while True:
    complete = json.loads(command('--limit', '10000', '--json').stdout)
    result = command('--json')
    data = json.loads(result.stdout)
    if len(complete['entries']) == 215 and len(data['entries']) == 100:
        break
    assert time.monotonic() < end, data
    time.sleep(.3)
assert data['truncated'] and b'more results exist' in result.stderr
assert len(json.loads(command('--size', '7', '--json').stdout)['entries']) == 7
assert len(json.loads(command('--limit', '10000', '--json').stdout)['entries']) == 215
expected = {os.fsencode(other / ('item-%03d.txt' % i)) for i in range(215)}
export = work / 'all-paths.bin'
assert command('--file', str(export), '-0').stdout == b''
assert set(export.read_bytes().split(b'\0')[:-1]) == expected
ndjson = work / 'all-paths.jsonl'
command('--json', '--file', str(ndjson))
records = [json.loads(line) for line in ndjson.read_bytes().splitlines()]
assert sum('entry' in r for r in records) == 215
assert records[-1] == {'complete': True, 'exported': 215}
command('--file', '-', expected=1)
command('--file', str(work), expected=1)
print('100 default, size/limit bounds, complete NUL and NDJSON export PASS', flush=True)

port = int(os.environ['EFS_QUERY_TEST_PORT'])
ino = root.stat().st_ino
fields = dict(root=ino, uid=os.getuid(), gid=os.getgid(), match=2, term=b'.txt')
r, raw = query(port, page_request(**fields, limit=1))
assert r.status == 0, r.status
first = json.loads(raw);token = int(first['page']['token']);cursor = int(first['page']['cursor'])
identity = (r.boot_id, r.generation)
seen = {e['path_hex'] for e in first['entries']}
r, _ = query(port, page_request(**fields, nonce=2))
assert r.status == -13, 'second retained export must refuse resource admission'
(root / 'new-after-pin.txt').write_bytes(b'new')
time.sleep(3)
while first['page']['more']:
    r, raw = query(port, page_request(**fields, token=token, cursor=cursor, limit=50))
    assert r.status == 0 and (r.boot_id, r.generation) == identity, (r.status, r.generation, identity)
    first = json.loads(raw);cursor = int(first['page']['cursor'])
    for entry in first['entries']:
        assert entry['path_hex'] not in seen, 'duplicate entry between pages'
        seen.add(entry['path_hex'])
assert len(seen) == 215 and b'new-after-pin.txt'.hex() not in seen
r, _ = query(port, page_request(**fields, token=token, cursor=cursor, limit=0))
assert r.status == 0
r, _ = query(port, page_request(**fields, token=token, cursor=cursor, limit=1))
assert r.status == -14, 'closed token must fail stale'
print('pinned generation survives refresh; no missing/duplicate entries; close/stale PASS', flush=True)

r, raw = query(port, page_request(**fields, limit=1))
assert r.status == 0
data = json.loads(raw);token = int(data['page']['token']);cursor = int(data['page']['cursor'])
try:
    root.chmod(0)
    r, _ = query(port, page_request(**fields, token=token, cursor=cursor, limit=1))
    assert r.status == -16, ('permission revocation', r.status)
finally:
    root.chmod(0o755)
    query(port, page_request(**fields, token=token, cursor=cursor, limit=0))
print('fresh permission checks across pages PASS', flush=True)
