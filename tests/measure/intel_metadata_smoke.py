#!/usr/bin/env python3
"""Bounded read-only mounted metadata timing; run unchanged before/after.

Usage: python3 intel_metadata_smoke.py MOUNT (existing Intel tiny-file fixture)
No warm-cache/loaded throughput claim; five serial 120-stat rounds and six
exact length/EOF read controls. No files created or mutated.
"""
import hashlib
import json
import os
import statistics
import subprocess
import sys
import time

mount = sys.argv[1]
assert subprocess.check_output(['findmnt', '-rn', '-M', mount, '-o', 'FSTYPE'], text=True).strip() == 'fuse.efs-fuse'
base = os.path.join(mount, 'tiny-intel2-1m-20261010')
paths = [os.path.join(base, x) for x in (
    'folder_115666/file_000000.txt', 'folder_115402/file_000000.txt',
    'folder_115402/file_000001.txt', 'file_000002.txt', 'file_000001.txt',
    'file_000000.txt')]
for p in paths:
    st = os.stat(p)
    with open(p, 'rb') as f:
        b = f.read(1025)
    assert len(b) == st.st_size and 1 <= len(b) <= 512
    print(json.dumps(dict(path=os.path.relpath(p, mount), bytes=len(b), sha256=hashlib.sha256(b).hexdigest())))
for r in range(5):
    latencies = []
    start = time.perf_counter_ns()
    for _ in range(20):
        for p in paths:
            a = time.perf_counter_ns()
            assert 1 <= os.stat(p).st_size <= 512
            latencies.append(time.perf_counter_ns()-a)
    elapsed = time.perf_counter_ns()-start
    ordered = sorted(latencies)
    print(json.dumps(dict(round=r, operations=len(latencies), elapsed_ms=elapsed/1e6,
        mean_us=statistics.mean(latencies)/1e3, median_us=statistics.median(latencies)/1e3,
        p95_us=ordered[int(.95*(len(ordered)-1))]/1e3)), flush=True)
