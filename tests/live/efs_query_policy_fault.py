#!/usr/bin/env python3
"""Private-port policy authority loss and lost-reply resolution oracles."""
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import time
from query_rpc import features, filtered_request, page_request, query, request

mount, mgmt, evidence = map(Path, sys.argv[1:4])
phase = sys.argv[4]
assert phase in ('loss', 'lost-reply', 'ceiling')
assert os.path.ismount(mount)
port = int(os.environ['EFS_QUERY_TEST_PORT'])
assert port == 19432
os.environ['EFS_QUERY_TEST_HOST'] = '192.168.122.111'
root = mount / ('w92-disabled-catchup-20261010' if phase == 'ceiling' else
                'w92-durable-policy-v6-20261010')
evidence.mkdir(exist_ok=True)

def policy(node, *args, status=0):
    result = subprocess.run([str(mgmt), 'commands-policy', node, *map(str, args)],
                            capture_output=True, text=True, timeout=30)
    match = re.search(r'rc=(-?\d+) version=(\d+) mask=(\d+) leader_hint=(-?\d+)', result.stdout)
    assert match, (result.stdout, result.stderr)
    rc, version, mask, leader = map(int, match.groups())
    assert rc == status, result.stdout
    return version, mask, leader

fields = dict(root=root.stat().st_ino, uid=os.getuid(), gid=os.getgid(),
              match=1, term=b'file-', max_output=1 << 20)
if phase == 'loss':
    assert features(port) == 3
    with open(root / '.efs/.query' / ('v1.stats.' + os.urandom(16).hex()), 'rb') as retained:
        original = retained.read()
        assert json.loads(original)['stats']['tree_files'] == 120
        ready = evidence / 'ready'
        assert not ready.exists() and not (evidence / 'start').exists()
        ready.write_text('ready\n')
        print('READY: stop private policy nodes 2/3, then create start marker', flush=True)
        end = time.monotonic() + 120
        while not (evidence / 'start').exists():
            assert time.monotonic() < end
            time.sleep(.1)
        end = time.monotonic() + 30
        while features(port):
            assert time.monotonic() < end, 'quorum loss still advertises commands'
            time.sleep(.1)
        requests = [request(**fields, version=version, operation=operation)
                    for version in (1, 2) for operation in (0, 1)]
        requests += [filtered_request(**fields), page_request(**fields)]
        verdicts = []
        for req in requests:
            reply, text = query(port, req)
            assert reply.status and not text
            verdicts.append(reply.status)
        retained.seek(0)
        assert retained.read() == original
    print(json.dumps(dict(result='PASS', phase=phase, features=0,
                          versions=[1, 2, 3, 4], verdicts=verdicts,
                          retained_bytes=len(original))), flush=True)
elif phase == 'lost-reply':
    seed = '192.168.122.111:19432'
    version, mask, leader = policy(seed, 'get')
    assert mask == 3 and 0 <= leader < 3
    body = bytes([0, 43]) + struct.pack('!QI', version, 3)
    with socket.create_connection(('192.168.122.%d' % (111 + leader), port), 7) as connection:
        connection.sendall(struct.pack('!I', len(body) + 1) + bytes([95]) + body)
        # Deliberately lose the reply after the complete request is sent.
    end = time.monotonic() + 30
    while True:
        current, mask, _ = policy(seed, 'get')
        if current == version + 1 and mask == 3:
            break
        assert current == version and time.monotonic() < end, (current, mask)
        time.sleep(.1)
    for node in (111, 112, 113):
        address = '192.168.122.%d:19432' % node
        policy(address, 'set', 3, version, status=-14)
        current, mask, _ = policy(address, 'get')
        assert current == version + 1 and mask == 3
    print(json.dumps(dict(result='PASS', phase=phase, version_before=version,
                          version_after=current, mask=mask)), flush=True)

else:
    assert features(port) == 3
    for node in (111, 112, 113):
        version, mask, _ = policy('192.168.122.%d:19432' % node, 'get')
        assert version == 18 and mask == 3
    import hashlib
    for index in range(500):
        expected = hashlib.sha256(('w92-disabled-%d' % index).encode()).digest() * 128
        assert (root / ('%04d.bin' % index)).read_bytes() == expected
    end = time.monotonic() + 180
    while True:
        result = subprocess.run([sys.executable, str(root / '.efs/stats'), '--json'],
                                capture_output=True, timeout=30)
        if not result.returncode:
            decoded = json.loads(result.stdout)
            if decoded['stats']['tree_files'] == 500:
                assert decoded['stats']['tree_bytes'] == 2048000
                assert decoded['snapshot']['refresh_error'] == 0
                break
        assert time.monotonic() < end, (result.returncode, result.stderr)
        time.sleep(.25)
    output = evidence / 'paths.nul'
    result = subprocess.run([sys.executable, str(root / '.efs/find'), '.bin', '--match', 'suffix',
                             '-0', '--file', str(output)], capture_output=True, timeout=120)
    assert not result.returncode, result.stderr
    names = output.read_bytes().split(b'\0')[:-1]
    assert len(names) == 500 and set(names) == {os.fsencode(root / ('%04d.bin' % i)) for i in range(500)}
    print(json.dumps(dict(result='PASS', phase=phase, version=version, mask=mask,
                          files=500, bytes=2048000, snapshot=decoded['snapshot'])), flush=True)
