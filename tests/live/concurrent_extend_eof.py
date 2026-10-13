#!/usr/bin/env python3
"""Repeat the existing high/low extension oracle on an active EFS mount.

No fork, retries, daemon restart or profile changes. Successful fixtures are
removed; failed fixtures stay for inspection. JSON on stdout is raw evidence.
Example: python3 tests/live/concurrent_extend_eof.py /tmp/efs-mount --cases 160 --workers 16
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'posix'))
import posix_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mount', type=Path)
    parser.add_argument('--cases', type=int, default=160)
    parser.add_argument('--workers', type=int, default=16)
    args = parser.parse_args()
    if not 1 <= args.cases <= 10000 or not 1 <= args.workers <= 64:
        parser.error('cases must be 1..10000 and workers 1..64')
    if not os.path.ismount(args.mount):
        parser.error('an active mount is required')
    root = Path(tempfile.mkdtemp(prefix='extend-eof-', dir=args.mount))
    started = time.monotonic()

    def one(index):
        directory = root / str(index)
        try:
            directory.mkdir()
            posix_suite.concurrent_extend_high_then_low(str(directory))
            shutil.rmtree(directory)
            return None
        except Exception as error:
            result = dict(case=index, error=repr(error), directory=str(directory))
            try:
                st = (directory / 'f').stat()
                result.update(inode=st.st_ino, actual_size=st.st_size,
                              expected_size=(8 << 20) + 1)
            except OSError as inspection_error:
                result['inspection_error'] = repr(inspection_error)
            return result

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        failures = [result for result in pool.map(one, range(args.cases)) if result]
    if not failures:
        root.rmdir()
    print(json.dumps(dict(result='FAIL' if failures else 'PASS', cases=args.cases,
                          passed=args.cases-len(failures), workers=args.workers,
                          seconds=time.monotonic()-started, fixture=str(root),
                          failures=failures)), flush=True)
    return int(bool(failures))


if __name__ == '__main__':
    sys.exit(main())
