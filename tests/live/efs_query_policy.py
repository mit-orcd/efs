#!/usr/bin/env python3
"""Run prepare, cold-restart this private namespace, then check. No production policy."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from query_rpc import features, page_request, query, request

mount, mgmt = map(Path, sys.argv[1:3])
phase = sys.argv[3]
assert phase in ('prepare', 'check') and os.path.ismount(mount)
port = int(os.environ['EFS_QUERY_TEST_PORT'])
assert port == 19432, 'private policy fixture port only'
os.environ.setdefault('EFS_QUERY_TEST_HOST', '192.168.122.111')
nodes = ['192.168.122.%d:%d' % (n, port) for n in (111, 112, 113)]
fixture = sys.argv[4] if len(sys.argv) > 4 else 'w92-durable-policy-20261010'
assert '/' not in fixture and fixture not in ('', '.', '..')
root = mount / fixture
base = int(sys.argv[5]) if len(sys.argv) > 5 else 0
assert 0 <= base < (1 << 64) - 5

def policy(node, *args, success=True):
    result = subprocess.run([str(mgmt), 'commands-policy', node, *map(str, args)],
                            capture_output=True, text=True, timeout=30)
    match = re.search(r'rc=(-?\d+) version=(\d+) mask=(\d+)', result.stdout)
    assert match, (result.stdout, result.stderr)
    rc, version, mask = map(int, match.groups())
    assert (rc == 0) == success and (result.returncode == 0) == success, result.stdout
    if not success:
        assert rc == -14, result.stdout
    return version, mask

def same(version, mask):
    for node in nodes:
        assert policy(node, 'get') == (version, mask)

def change(version, mask):
    assert policy(nodes[1], 'set', mask, version) == (version + 1, mask)
    same(version + 1, mask)
    policy(nodes[2], 'set', 3, version, success=False)
    same(version + 1, mask)

def data(index):
    return hashlib.sha256(('w92-policy:%d' % index).encode()).digest() * 128

def write(begin, end):
    for i in range(begin, end):
        with open(root / ('file-%03d.bin' % i), 'xb') as stream:
            stream.write(data(i));stream.flush();os.fsync(stream.fileno())

def stats(count):
    deadline = time.monotonic() + 180
    while True:
        result = subprocess.run([sys.executable, str(root / '.efs/stats'), '--json'],
                                capture_output=True, timeout=30)
        if result.returncode == 0:
            decoded = json.loads(result.stdout)
            if decoded['stats']['tree_files'] == count:
                assert decoded['stats']['tree_bytes'] == count * 4096
                assert decoded['snapshot']['refresh_error'] == 0
                return decoded
        assert time.monotonic() < deadline, (result.returncode, result.stderr)
        time.sleep(.25)

fields = dict(root=1, uid=os.getuid(), gid=os.getgid(), version=2, match=1,
              term=b'file-', max_output=1 << 20)
if phase == 'prepare':
    same(base, 3)
    root.mkdir();write(0, 20);stats(20)
    fields['root'] = root.stat().st_ino
    with open(root / '.efs/.query' / ('v1.stats.' + os.urandom(16).hex()), 'rb') as retained:
        original = retained.read()
        reply, text = query(port, page_request(**{k:v for k,v in fields.items() if k!='version'}, limit=1))
        assert reply.status == 0
        page = json.loads(text)['page']
        assert page['more']
        change(base, 0)
        assert features(port) == 0
        for version in (1, 2):
            for operation in (0, 1):
                reply, text = query(port, request(**dict(fields, version=version, operation=operation)))
                assert reply.status and not text
        reply, text = query(port, page_request(**{k:v for k,v in fields.items() if k!='version'},
                            token=int(page['token']), cursor=int(page['cursor']), limit=1))
        assert reply.status and not text
        reply, _ = query(port, page_request(**{k:v for k,v in fields.items() if k!='version'},
                            token=int(page['token']), cursor=int(page['cursor']), limit=0))
        assert not reply.status, 'disabled export must still release its lease'
        write(20, 120)
        retained.seek(0);assert retained.read() == original
    change(base + 1, 1)
    assert features(port) == 1
    stats(120)
    for operation in (1, 2):
        reply, text = query(port, request(**dict(fields, operation=operation)))
        assert reply.status and not text
    change(base + 2, 2)
    assert features(port) == 2
    reply, text = query(port, request(**dict(fields, operation=0)))
    assert reply.status and not text
    for operation in (1, 2):
        reply, text = query(port, request(**dict(fields, operation=operation)))
        assert not reply.status and text, reply.status
    change(base + 3, 0)
else:
    same(base + 4, 0)
    assert features(port) == 0
    assert len(list(root.iterdir())) == 120
    for i in range(120):
        assert (root / ('file-%03d.bin' % i)).read_bytes() == data(i)
    change(base + 4, 3)
    stats(120)
    output = mount.parent / 'w92-policy-paths.nul'
    result = subprocess.run([sys.executable, str(root / '.efs/find'), 'file-', '--match', 'prefix',
                             '-0', '--file', str(output)], capture_output=True, timeout=120)
    assert not result.returncode, result.stderr
    exported = output.read_bytes().split(b'\0')[:-1]
    assert len(exported) == 120 and set(exported) == {
        os.fsencode(root / ('file-%03d.bin' % i)) for i in range(120)}
print(json.dumps(dict(result='PASS', phase=phase, files=120, bytes=120*4096)), flush=True)
