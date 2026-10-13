#!/usr/bin/env python3
"""Diagnostic CREATE/write/close timing; not a POSIX qualification gate."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mount', type=Path)
parser.add_argument('--count', type=int, default=50)
parser.add_argument('--empty', action='store_true', help='measure committed empty CREATE/close without DATA')
args = parser.parse_args()
if not 1 <= args.count <= 1000:
    parser.error('--count must be between 1 and 1000')
root = Path(tempfile.mkdtemp(prefix='efs-small-phases-', dir=args.mount))
samples = {key: [] for key in ('open', 'write_flush', 'close')}
complete = False
try:
    for i in range(args.count):
        payload = b"" if args.empty else hashlib.blake2b(str(i).encode()).digest()
        start = time.perf_counter_ns()
        file = open(root / str(i), 'xb')
        opened = time.perf_counter_ns()
        try:
            file.write(payload)
            file.flush()  # send buffered bytes; deliberately no added fsync
            flushed = time.perf_counter_ns()
        finally:
            file.close()
        closed = time.perf_counter_ns()
        for phase, elapsed in zip(samples, (opened-start, flushed-opened, closed-flushed)):
            samples[phase].append(elapsed / 1e6)
    for i in range(args.count):
        assert (root / str(i)).read_bytes() == (b"" if args.empty else hashlib.blake2b(str(i).encode()).digest())
    complete = True
    result = {'kind': 'diagnostic', 'count': args.count, 'payload_bytes': 0 if args.empty else 64,
              'added_fsync': False, 'fixture': str(root), 'content_verified': True}
    result['milliseconds'] = {}
    for phase, values in samples.items():
        ordered = sorted(values)
        result['milliseconds'][phase] = {
            'mean': statistics.mean(values), 'median': statistics.median(values),
            'p95': ordered[min(len(ordered)-1, (95*len(ordered)+99)//100-1)],
            'min': min(values), 'max': max(values)}
    print(json.dumps(result, indent=2), flush=True)
finally:
    if complete:
        for i in range(args.count):
            (root / str(i)).unlink()
        root.rmdir()
    else:
        print('incomplete diagnostic; retained fixture:', root, flush=True)
