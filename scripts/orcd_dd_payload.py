#!/usr/bin/env python3
"""Prepare/verify every byte of a bounded nonzero post-fsync/restart oracle.

Invoke prepare on one mounted client, verify on a different client before
and after a checked cluster restart. This is correctness, not a dd timing.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import time

BLOCK = 1 << 20
SEED = 0x454653444450524F


def image(buf, index):
    struct.pack_into('<QQ', buf, 0, SEED, index)
    struct.pack_into('<QQ', buf, len(buf)-16, index ^ 0xA5A5A5A5A5A5A5A5, SEED)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('phase', choices=('prepare', 'verify'))
    ap.add_argument('file', type=Path)
    ap.add_argument('--mib', type=int, default=1024)
    a = ap.parse_args()
    assert 1 <= a.mib <= 8192
    expected = bytearray([0x3C]) * BLOCK
    start = time.monotonic()
    if a.phase == 'prepare':
        with a.file.open('xb', buffering=0) as f:
            for i in range(a.mib):
                image(expected, i)
                view = memoryview(expected)
                while view:
                    n = f.write(view)
                    assert n and n > 0, (i, n)
                    view = view[n:]
            os.fdatasync(f.fileno())
    else:
        assert a.file.stat().st_size == a.mib * BLOCK
        with a.file.open('rb') as f:
            for i in range(a.mib):
                image(expected, i)
                got = f.read(BLOCK)
                assert got == expected, ('payload mismatch', i, len(got), got[:16].hex())
            assert not f.read(1), 'unexpected suffix'
    assert a.file.stat().st_size == a.mib * BLOCK
    print(json.dumps(dict(phase=a.phase, file=str(a.file), bytes=a.mib*BLOCK,
                          host=os.uname().nodename, seconds=time.monotonic()-start,
                          status='PASS')), flush=True)


if __name__ == '__main__':
    main()
