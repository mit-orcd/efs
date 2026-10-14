#!/usr/bin/env python3
"""Retained-file oracle for write/chmod/ftruncate through an existing descriptor.

No application retries or profile changes. Keep every attempted file, including
failures, for inspection. --verify checks a successful fixture after restart.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('mount', type=Path)
p.add_argument('--files', type=int, default=500)
p.add_argument('--workers', type=int, default=16)
p.add_argument('--verify', type=Path)
a = p.parse_args()
if not os.path.ismount(a.mount):
    p.error('active mount required')
if not 1 <= a.files <= 100000 or not 1 <= a.workers <= 64:
    p.error('files must be 1..100000, workers 1..64')
root = a.verify or Path(tempfile.mkdtemp(prefix='chmod-truncate-', dir=a.mount))
if root.parent.resolve() != a.mount.resolve() or not root.name.startswith('chmod-truncate-'):
    p.error('owned fixture directly below mount required')
start = time.monotonic()


def one(i):
    path = root / str(i)
    fd = None
    phase = 'verify' if a.verify else 'open'
    ino = None
    result = None
    try:
        if not a.verify:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o644)
            ino = os.fstat(fd).st_ino
            phase = 'write'
            assert os.write(fd, b'ABCDEFGH') == 8
            phase = 'chmod-ro'
            os.chmod(path, 0o444)
            phase = 'ftruncate'
            os.ftruncate(fd, 3)
            phase = 'close'
            closing, fd = fd, None  # close errors must not close a reused fd
            os.close(closing)
            phase = 'chmod-rw'
            os.chmod(path, 0o644)
        phase = 'verify'
        assert path.stat().st_size == 3 and path.read_bytes() == b'ABC'
    except Exception as error:
        result = dict(file=i, inode=ino, phase=phase, error=repr(error))
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError as error:
                if result is None:
                    result = dict(file=i, inode=ino, phase='cleanup-close', error=repr(error))
                else:
                    result['cleanup_error'] = repr(error)
    return result


with ThreadPoolExecutor(a.workers) as pool:
    failures = [v for v in pool.map(one, range(a.files)) if v]
print(json.dumps(dict(result='FAIL' if failures else 'PASS', files=a.files,
                      workers=a.workers, fixture=str(root), cold_verify=bool(a.verify),
                      seconds=time.monotonic()-start, failures=failures)), flush=True)
raise SystemExit(bool(failures))
