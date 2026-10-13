#!/usr/bin/env python3
"""Concurrent ordinary-file create/write/fsync oracle; retain failed fixtures.

Run against an existing mount without restarting or changing its profile.
Example: python3 tests/live/fsync_admission.py /data1/efs/mnt --files 1025 --workers 16
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import tempfile
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mount', type=Path)
    ap.add_argument('--files', type=int, default=1025)
    ap.add_argument('--workers', type=int, default=16)
    args = ap.parse_args()
    if not os.path.ismount(args.mount):
        ap.error('an active mount is required')
    if not 1 <= args.files <= 100000 or not 1 <= args.workers <= 64:
        ap.error('files must be 1..100000; workers 1..64')
    root = Path(tempfile.mkdtemp(prefix='fsync-admission-', dir=args.mount))
    start = time.monotonic()

    def one(i):
        path = root / str(i)
        payload = i.to_bytes(8, 'little') * 4
        phase = 'create'
        try:
            with path.open('xb') as f:
                phase = 'write'
                if f.write(payload) != len(payload):
                    raise AssertionError('short write')
                phase = 'flush'
                f.flush()
                phase = 'fsync'
                os.fsync(f.fileno())
                phase = 'close'
            phase = 'verify'
            if path.stat().st_size != len(payload) or path.read_bytes() != payload:
                raise AssertionError('EOF or contents mismatch')
            phase = 'unlink'
            path.unlink()
            return None
        except Exception as error:
            result = dict(file=i, path=str(path), phase=phase, error=repr(error))
            try:
                st = path.stat()
                result.update(inode=st.st_ino, size=st.st_size)
            except OSError as inspection:
                result['inspection_error'] = repr(inspection)
            return result

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        failures = [r for r in pool.map(one, range(args.files)) if r]
    if not failures:
        root.rmdir()
    print(json.dumps(dict(result='FAIL' if failures else 'PASS', files=args.files,
                          workers=args.workers, seconds=time.monotonic()-start,
                          fixture=str(root), failures=failures)), flush=True)
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
