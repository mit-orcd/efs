#!/usr/bin/env python3
"""Diagnostic timing of the POSIX 80-append mtime pattern; not an acceptance gate.
Keeps its fixture and emits each operation before the next can block.
"""
import argparse
import json
import os
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('mount')
a = p.parse_args()
root = tempfile.mkdtemp(prefix='mtime-diagnostic-', dir=a.mount)
path = os.path.join(root, 'f')
start = time.monotonic()
print(json.dumps({'fixture': root}), flush=True)
def timed(name, fn, iteration):
    before = time.monotonic()
    result = fn()
    print(json.dumps({'iteration': iteration, 'operation': name,
        'seconds': time.monotonic()-before, 'elapsed': time.monotonic()-start}), flush=True)
    return result
with open(path, 'wb') as f:
    f.write(b'x')
last = os.stat(path).st_mtime_ns
for i in range(80):
    f = timed('open', lambda: open(path, 'ab'), i)
    timed('write', lambda: f.write(b'y'), i)
    timed('close', f.close, i)
    now = timed('stat', lambda: os.stat(path).st_mtime_ns, i)
    assert now >= last, 'mtime went backwards at append %d' % i
    last = now
print(json.dumps({'complete': True, 'appends': 80, 'seconds': time.monotonic()-start}), flush=True)
