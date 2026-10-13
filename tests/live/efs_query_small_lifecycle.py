#!/usr/bin/env python3
"""Deterministic .efs payload/path oracle: prepare, orderly restart, then check.
Use an exclusively owned test namespace; scripts never erase unrelated data.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('mount', type=Path)
p.add_argument('peer', type=Path)
p.add_argument('phase', choices=('prepare', 'check', 'cleanup'))
a = p.parse_args()
name = 'package2-deterministic-lifecycle'
root, peer = a.mount/name, a.peer/name

def payload(i):
    return hashlib.sha256(('package2:%d' % i).encode()).digest()*129

def query():
    r = subprocess.run([sys.executable, str(peer/'.efs/find'), '', '--json', '--limit', '10000'],
                       capture_output=True, timeout=30)
    assert r.returncode == 0, (r.returncode, r.stderr)
    data = json.loads(r.stdout)
    assert data['complete'] and not data.get('truncated', False), data
    return {bytes.fromhex(e['path_hex']) for e in data['entries']}

def check():
    expected = {('dir-%02d/file-%03d' % (i%8, i)).encode() for i in range(256)}
    expected |= {('mutated-%03d' % i).encode() for i in range(32)}
    expected |= {('dir-%02d' % i).encode() for i in range(8)}
    deadline = time.monotonic()+25
    while query() != expected:
        assert time.monotonic() < deadline, 'fresh query path mismatch'
        time.sleep(.2)
    for i in range(256):
        rel = 'dir-%02d/file-%03d' % (i%8, i)
        assert (root/rel).read_bytes() == payload(i)
        assert (peer/rel).read_bytes() == payload(i)
    for i in range(32):
        assert (peer/('mutated-%03d' % i)).read_bytes() == payload(i+256)
    for mode in ([], ['--human'], ['--json']):
        r = subprocess.run([sys.executable, str(peer/'.efs/stats'), *mode],
                           capture_output=True, timeout=30)
        assert r.returncode == 0 and r.stdout, (mode, r.stderr)
    print('complete 296-path/1,188,864-byte dual-mount payload and stats oracle PASS', flush=True)

if a.phase == 'prepare':
    root.mkdir()
    for i in range(8): (root/('dir-%02d' % i)).mkdir()
    for i in range(256):
        with (root/('dir-%02d/file-%03d' % (i%8, i))).open('wb') as f:
            f.write(payload(i)); f.flush(); os.fsync(f.fileno())
    query()
    def reader():
        for _ in range(8): query()
    def writer():
        for i in range(32):
            temp = root/('pending-%03d' % i)
            with temp.open('wb') as f:
                f.write(payload(i+256)); f.flush(); os.fsync(f.fileno())
            temp.rename(root/('mutated-%03d' % i))
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        futures = [pool.submit(reader) for _ in range(4)] + [pool.submit(writer)]
        for future in futures: future.result()
    print('32 concurrent queries during fsync/rename PASS', flush=True)
    check()
elif a.phase == 'check':
    check()
else:
    import shutil
    shutil.rmtree(root)
    print('owned deterministic fixture removed', flush=True)
