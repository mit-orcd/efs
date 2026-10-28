#!/usr/bin/env python3
"""K52 diagnostic: exact D3, with fork-launching queries and isolated reads.

Run on a private mounted fixture. Modes are diagnostic controls, not product
settings. The original query launch deliberately reproduces the W121 hazard.
No mismatch is retried; files and first-read evidence remain on failure.
"""
import argparse
import errno
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import threading

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('stage2_helpers', root / 'scripts/stage2_helpers.py')
H = importlib.util.module_from_spec(spec)
spec.loader.exec_module(H)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mount', type=Path)
    p.add_argument('name')
    p.add_argument('--reader', choices=('in-process', 'spawn'), required=True)
    p.add_argument('--load-s', type=float, default=300)
    a = p.parse_args()
    if not os.path.ismount(a.mount) or Path(a.name).name != a.name:
        p.error('active private mount and a single fresh fixture name required')
    previous = threading.local()
    def verify(path, expected):
        if a.reader == 'spawn':
            rc, observed, err, _ = H.spawn_capture([
                sys.executable, '-c',
                'import pathlib,sys;sys.stdout.buffer.write(pathlib.Path(sys.argv[1]).read_bytes())',
                str(path)], 10)
            if rc:
                raise OSError(errno.EIO, f'spawned reader rc={rc}: {err}')
        else:
            observed = Path(path).read_bytes()
        old = getattr(previous, 'expected', None)
        previous.expected = expected
        if observed != expected:
            exc = OSError(errno.EIO, 'read-back mismatch')
            exc.payload_detail = dict(path=str(path), expected_len=len(expected), observed_len=len(observed),
                expected_sha256=hashlib.sha256(expected).hexdigest(),
                observed_sha256=hashlib.sha256(observed).hexdigest(),
                previous_expected_len=len(old) if old is not None else None,
                previous_expected_sha256=hashlib.sha256(old).hexdigest() if old is not None else None,
                observed_equals_previous_expected=observed == old,
                first_mismatch=next((i for i,(x,y) in enumerate(zip(expected,observed)) if x != y),
                                    min(len(expected),len(observed))), reader=a.reader,
                phase='after write/fsync/close', pid=os.getpid())
            raise exc
    def original_query(where, cmd, *args, timeout=60):
        t0 = H.time.monotonic()
        try:
            r = subprocess.run([sys.executable, str(where / '.efs' / cmd), *args], cwd='/',
                               capture_output=True, timeout=timeout)
            return r.returncode, r.stdout, r.stderr.decode(errors='replace'), H.time.monotonic()-t0
        except subprocess.TimeoutExpired:
            return 124, b'', 'deadline', H.time.monotonic()-t0
    H.verify_payload = verify
    H.efs_cmd = original_query
    return H.h_d3(argparse.Namespace(dir=str(a.mount/a.name), mount=str(a.mount), peer=None,
        seed=a.name, static_files=200, writers=8, queriers=4, load_s=a.load_s,
        io_deadline_s=10, query_deadline_s=30, index_wait_s=600))


if __name__ == '__main__':
    raise SystemExit(main())
