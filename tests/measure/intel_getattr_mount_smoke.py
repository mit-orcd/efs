#!/usr/bin/env python3
"""Bounded four-client GETATTR/data smoke, operating only on a new owned directory.

Run create for roles0..3, then read on all mounts, then clean once. All clients
must use the same RUN name. No daemon lifecycle or feature settings are changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def expected(role):
    original = bytes(range(256))*1024+bytes([role])*17
    return original[:4099]+bytes(65536-4099)+bytes([65+role])*17


def verify(root):
    for role in range(4):
        p = root/f'writer-{role}'
        with p.open('rb') as f:
            got = f.read(65554)
            assert f.read(1) == b''
        st = p.stat()
        assert got == expected(role) and st.st_size == 65553
        assert st.st_nlink == 1 and st.st_mtime_ns == 1700000000000000000+role*1000


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mount', type=Path)
    ap.add_argument('mode', choices=['create', 'read', 'clean'])
    ap.add_argument('run')
    ap.add_argument('role', type=int, choices=range(4))
    a = ap.parse_args()
    assert re.fullmatch(r'getattr-lane-smoke-[a-zA-Z0-9-]+', a.run)
    assert subprocess.check_output(['findmnt', '-rn', '-M', str(a.mount), '-o', 'FSTYPE'], text=True).strip() == 'fuse.efs-fuse'
    assert not os.path.lexists(a.mount/'.efs')
    root = a.mount/a.run
    if a.mode == 'create':
        root.mkdir(exist_ok=True)
        assert not root.is_symlink()
        p = root/f'writer-{a.role}'
        original = bytes(range(256))*1024+bytes([a.role])*17
        with p.open('xb') as f:
            f.write(original)
            f.flush()
            os.fsync(f.fileno())
        assert p.stat().st_size == len(original)
        with p.open('r+b', buffering=0) as f:
            f.truncate(4099)
            os.fsync(f.fileno())
            assert os.fstat(f.fileno()).st_size == 4099
            f.truncate(65553)
            f.seek(65536)
            assert f.write(bytes([65+a.role])*17) == 17
            os.fsync(f.fileno())
        stamp = 1700000000000000000+a.role*1000
        os.utime(p, ns=(stamp, stamp))
        assert p.stat().st_size == 65553 and p.stat().st_mtime_ns == stamp
        print(json.dumps(dict(role=a.role, bytes=65553,
                              sha256=hashlib.sha256(expected(a.role)).hexdigest())))
    else:
        verify(root)
        if a.mode == 'clean':
            assert a.role == 0 and not root.is_symlink()
            assert {p.name for p in root.iterdir()} == {f'writer-{i}' for i in range(4)}
            for i in range(4):
                (root/f'writer-{i}').unlink()
            root.rmdir()
        print(json.dumps(dict(role=a.role, mode=a.mode, verified=4,
                              bytes=True, EOF=True, size=True, mtime=True)))


if __name__ == '__main__':
    main()
