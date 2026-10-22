#!/usr/bin/env python3
"""Client-path and command/export oracle, including macOS over an NFS gateway.

Run only on a deliberately opted-in test export. The caller records the NFS
credential mapping separately; a root-squashed export is not a UID-501 ACL test.
Creates a fresh fixture and leaves it intact. Output exports stay on local disk.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

mount = Path(sys.argv[1]).resolve()
assert os.path.ismount(mount), 'refuse to create a fixture outside a mounted export'
assert (mount / '.efs/find').read_bytes().startswith(b'#!/usr/bin/env python3')
fixture = 'w92-nfs-%d-%d' % (int(time.time()), os.getpid())
root = mount / fixture
root.mkdir()
(root / 'child').mkdir()
expected = {}
for i in range(137):
    relative = ('child/' if i % 2 else '') + 'image-%03d.dat' % i
    payload = hashlib.sha256(('w92-nfs:%d' % i).encode()).digest() * 128
    with open(root / relative, 'xb') as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    expected[os.fsencode(relative)] = hashlib.sha256(payload).hexdigest()
for name in (b'line\nbreak.dat', b'escape\x1b.dat', b'invalid\xff.dat'):
    with open(os.fsencode(root) + b'/' + name, 'xb') as stream:
        stream.write(b'x')
        stream.flush()
        os.fsync(stream.fileno())
    expected[name] = hashlib.sha256(b'x').hexdigest()

def command(name, *args):
    result = subprocess.run([sys.executable, str(root / '.efs' / name), *args],
                            cwd='/', capture_output=True, timeout=120)
    assert result.returncode == 0, (name, args, result.returncode, result.stderr)
    return result.stdout

assert (root / '.efs/find').read_bytes().startswith(b'#!/usr/bin/env python3')
assert '.efs' not in os.listdir(root)
assert b'List immediate children' in command('help')
deadline = time.monotonic() + 120
while True:
    try:
        stats = json.loads(command('stats', '--json'))
        assert stats['stats']['tree_files'] == len(expected), stats
        assert stats['stats']['tree_dirs'] == 1, stats
        assert stats['stats']['tree_bytes'] == 137 * 4096 + 3, stats
        assert stats['snapshot']['refresh_error'] == 0, stats
        break
    except (AssertionError, OSError):
        if time.monotonic() >= deadline:
            raise
        time.sleep(.25)
# Pin the delivered old bytes, then require a NEW command to converge after a
# peer-visible mutation. This catches NFS retaining a fixed-name virtual result.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from query_rpc import query_name
with open(root / '.efs/.query' / query_name('stats'), 'rb') as pinned:
    old = pinned.read()
    assert json.loads(old)['stats']['tree_files'] == len(expected)
    payload = b'after-pinned-result'
    with open(root / 'after-pinned.dat', 'xb') as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    expected[b'after-pinned.dat'] = hashlib.sha256(payload).hexdigest()
    deadline = time.monotonic() + 120
    while True:
        stats = json.loads(command('stats', '--json'))
        if stats['stats']['tree_files'] == len(expected):
            assert stats['stats']['tree_bytes'] == 137 * 4096 + 3 + len(payload), stats
            break
        assert time.monotonic() < deadline, stats
        time.sleep(.25)
    pinned.seek(0)
    assert pinned.read() == old, 'an acquired NFS result changed across refresh'
listing = json.loads(command('find', '.dat', '--match', 'suffix', '--json', '--limit', '10000'))
assert listing['complete'], listing
assert {bytes.fromhex(row['path_hex']) for row in listing['entries']} == set(expected), listing
absolute = {os.fsencode(root) + b'/' + relative for relative in expected}
assert set(command('find', '.dat', '--match', 'suffix', '-0', '--limit', '10000').split(b'\0')[:-1]) == absolute
with tempfile.TemporaryDirectory(prefix='w92-nfs-export-') as directory:
    output = Path(directory) / 'paths'
    command('find', '.dat', '--match', 'suffix', '-0', '--file', str(output))
    exported = output.read_bytes().split(b'\0')[:-1]
    assert len(exported) == len(expected) and set(exported) == absolute
    for path in exported:
        relative = path[len(os.fsencode(root)) + 1:]
        with open(path, 'rb') as stream:
            assert hashlib.sha256(stream.read()).hexdigest() == expected[relative]
print(json.dumps(dict(result='PASS', platform=sys.platform, fixture=str(root),
    files=len(expected), payload_bytes=137 * 4096 + 3 + len(payload),
    gates=['source', 'lookup-only', 'help', 'stats', 'json', 'nul',
           'new-command-convergence', 'retained-result-bytes',
           'paged-export-client-paths', 'all-payload-hashes'], snapshot=stats['snapshot'])), flush=True)
