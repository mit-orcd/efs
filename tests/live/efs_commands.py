#!/usr/bin/env python3
"""Two mounted-export command gates. Leaves its dedicated test tree for evidence."""
import json
import errno
import os
from pathlib import Path
import subprocess
import sys
import time

mount, peer = map(Path, sys.argv[1:3])
name = 'efs-command-gates-%d' % os.getpid()
root, other = mount / name, peer / name
root.mkdir()
(root / 'child').mkdir()
(root / 'file.txt').write_bytes(b'payload')
(root / 'child/nested.txt').write_bytes(b'child')
raw_names = [b'line\nbreak.txt', b'escape\x1b.txt', b'invalid\xff.txt', b'$(touch PWNED).txt']
for raw in raw_names:
    with open(os.fsencode(root) + b'/' + raw, 'wb') as f:
        f.write(b'x')

def command(where, cmd, *args, expected=0):
    result = subprocess.run([sys.executable, str(where / '.efs' / cmd), *args], cwd='/',
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    assert result.returncode == expected, (cmd, args, result.returncode, result.stderr)
    return result

def eventually(check, timeout=20):
    deadline = time.monotonic() + timeout
    while True:
        try:
            check()
            return
        except (AssertionError, OSError):
            if time.monotonic() > deadline:
                raise
            time.sleep(.2)

def contents(where):
    result = command(where, 'find', '.txt', '--match', 'suffix', '--json')
    return json.loads(result.stdout)

source = (root / '.efs/find').read_bytes()
assert source.startswith(b'#!/usr/bin/env python3') and b'def main(' in source
assert set(os.listdir(root / '.efs')) == {'help', 'ls', 'find', 'stats'}
assert '.efs' not in os.listdir(root), 'synthetic command directory leaked into user traversal'
assert b'List immediate children' in command(root, 'help').stdout
direct = subprocess.run([str(root / '.efs/help')], cwd='/', stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=30)
assert direct.returncode == 0 and b'List immediate children' in direct.stdout, direct
print('source reads, command listing, lookup-only traversal and help PASS', flush=True)
expected = {b'file.txt', b'child/nested.txt', *raw_names}
def find_gate():
    result = contents(other)
    assert result['complete'] is True and result['expiry'] == 'unavailable', result
    assert {bytes.fromhex(e['path_hex']) for e in result['entries']} == expected, result
    assert result['snapshot']['refresh_n'] == 256, result
    build = result['build']
    assert build['kind'] in ('build', 'restore'), build
    assert all(isinstance(build[k], int) and build[k] >= 0 for k in
               ('pin_ms', 'total_ms', 'index_ns', 'stats_ns', 'persist_ns', 'restore_ns')), build
    if build['kind'] == 'restore':
        assert build['restore_ns'] > 0 and not any(build[k] for k in
               ('index_ns', 'stats_ns', 'persist_ns')), build
    else:
        assert build['index_ns'] > 0 and build['stats_ns'] > 0 and build['restore_ns'] == 0, build
    assert result['snapshot']['refresh_ms'] == int(os.environ.get('EFS_QUERY_REFRESH_MS', '60000')), result
eventually(find_gate)
result = command(other, 'find', '.txt', '--match', 'suffix', '-0')
assert set(result.stdout.split(b'\0')[:-1]) == {os.fsencode(other) + b'/' + p for p in expected}
assert b'\x1b' not in command(root, 'ls').stdout
ls = json.loads(command(root, 'ls', '--json').stdout)
assert {bytes.fromhex(e['path_hex']) for e in ls['entries']} == {b'child', b'file.txt', *raw_names}
print('peer catalog, raw filenames, JSON, NUL, terminal escaping and immediate listing PASS', flush=True)

def stats_gate():
    stats = json.loads(command(other, 'stats', '--json').stdout)['stats']
    assert stats['tree_files'] == len(expected) and stats['tree_dirs'] == 1, stats
    assert stats['tree_bytes'] == 16, stats
eventually(stats_gate)
def open_query(path):
    # ESTALE before OPEN succeeds is an explicit lookup/content-version race.
    # Once acquired, a descriptor must remain stable without retries.
    for attempt in range(8):
        try:
            return open(path,'rb')
        except OSError as error:
            if error.errno not in (errno.ESTALE,errno.EAGAIN,errno.EBUSY,errno.ENODATA) or attempt==7:
                raise
            time.sleep(.025*(attempt+1))

# Open a query result directly: multi-read/seek/fstat remains immutable.
# Exact probes keep immutable-FD/slot gates independent of the legacy global
# scan ceiling; broad-result behavior is covered by the v4 page gates.
query = root / '.efs/.query/v1.find.0.66696c652e747874'
with open_query(query) as stream:
    first = stream.read(17)
    size = os.fstat(stream.fileno()).st_size
    (other / 'new.txt').write_bytes(b'new')
    time.sleep(2.5)
    tail = stream.read()
    assert len(first + tail) == size
    stream.seek(0)
    assert stream.read() == first + tail
    assert os.fstat(stream.fileno()).st_size == size
print('truthful stats and immutable open/seek/fstat across peer mutation PASS', flush=True)
# Caller is the unprivileged mount owner. Reauthorize each new delivery;
# a descriptor already opened keeps its immutable bytes after chmod.
assert os.geteuid() != 0, 'permission gates must not run as root'
with open_query(query) as held:
    old = held.read()
    os.chmod(other, 0)
    try:
        # Search permission denies opening the script itself. Python reports
        # this as exit 2 before command.main() can return its query-error 1.
        denied = command(root, 'stats', '--json', expected=2)
        assert not denied.stdout and b'Permission denied' in denied.stderr, denied
        try:
            os.open(root / '.efs/stats', os.O_RDONLY)
        except OSError as error:
            assert error.errno == errno.EACCES, error
        else:
            raise AssertionError('revoked directory still allowed a fresh script open')
        held.seek(0)
        assert held.read() == old
    finally:
        os.chmod(other, 0o755)
os.chmod(other / 'child', 0)
try:
    denied = command(root, 'find', 'nested', '--json', expected=1)
    assert not denied.stdout, denied
finally:
    os.chmod(other / 'child', 0o755)
print('fresh caller/descendant permissions and authorized open lifetime PASS', flush=True)
# Symlink invocation resolves to the containing command directory.
link = root / 'find-link'
link.symlink_to('.efs/find')
result = subprocess.run([sys.executable, str(link), 'nested', '-0'], cwd='/',
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
assert result.returncode == 0 and result.stdout == os.fsencode(root) + b'/child/nested.txt\0', result
# Real user entries have precedence, and retired names are not reserved.
for legacy in ('.find', '.stats'):
    (root / legacy).write_bytes(b'user data')
    assert (other / legacy).read_bytes() == b'user data'
collision = root / 'collision'
collision.mkdir()
# Install the pre-existing real entry through the metadata API: an enabled
# mount correctly sees an absent .efs as a synthetic directory already.
mgmt = Path(__file__).resolve().parents[2] / 'efs-mgmt'
port = int(os.environ['EFS_QUERY_TEST_PORT'])
for candidate in range(port-1, port+3):
    created = subprocess.run([str(mgmt), 'raft-create', '%s:%d' % (os.environ.get('EFS_QUERY_TEST_HOST','127.0.0.1'),candidate),
                              str(collision.stat().st_ino), '.efs'], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=15)
    if created.returncode == 0:
        break
else:
    raise AssertionError(('could not install collision fixture', created.stdout, created.stderr))
assert (collision / '.efs').is_file()
assert (peer / name / 'collision/.efs').read_bytes() == b''
print('symlink scope, legacy user names and real .efs collision precedence PASS', flush=True)
# Exhaust distinct immutable identities, then prove release/FORGET recovery.
import errno
held=[]
try:
    for i in range(80):
        pattern=('capacity-%03d'%i).encode().hex()
        try:
            held.append(open_query(root / '.efs/.query' / ('v1.find.0.'+pattern)))
        except OSError as error:
            assert error.errno == errno.ENOMEM, error
            break
    else:
        raise AssertionError('immutable result identity cap was not enforced')
    assert len(held) >= 20, len(held)
finally:
    for stream in held:
        stream.close()
def recovered():
    with open_query(root / '.efs/.query/v1.find.0.7265636f7665726564') as stream:
        assert json.load(stream)['complete'] is True
eventually(recovered)
for i in range(128):
    with open_query(root / '.efs/.query' / ('v1.find.0.'+('released-%03d'%i).encode().hex())) as stream:
        assert json.load(stream)['complete'] is True
print('bounded identity exhaustion and sequential release/FORGET recovery PASS', flush=True)
print('efs_commands: PASS (focused command gates; large-scale/fault/NFS acceptance remains)', flush=True)
