#!/usr/bin/env python3
"""W1 N-1 shared-file repro (docs/archive/project-history.md, "START-HERE closed items").

Two clients concurrently pwrite disjoint 4 KiB ranges that share 128 KiB
chunks, fsync after each write. A third client remounts and counts 4 KiB
blocks that are not the owner's pattern. I12 says 0 lost.

Roles (env or argv):
  prepare  — create and pre-size the file (run on A)
  write-a  — 0xAA at i*8192
  write-b  — 0xBB at i*8192+4096
  verify   — count lost blocks (run after remount)
"""
from __future__ import print_function

import os
import sys

N = int(os.environ.get("EFS_N1_N", "500"))
BLK = 4096
STRIDE = 8192
FILE_SIZE = 64 * 1024 * 1024
NAME = os.environ.get("EFS_N1_NAME", "n1-shared")


def path(mnt):
    return os.path.join(mnt, NAME)


def prepare(mnt):
    p = path(mnt)
    fd = os.open(p, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o644)
    try:
        os.ftruncate(fd, FILE_SIZE)
        os.fsync(fd)
    finally:
        os.close(fd)
    print("PREPARE %s n=%d size=%d" % (p, N, FILE_SIZE))


def write_side(mnt, which):
    p = path(mnt)
    fd = os.open(p, os.O_RDWR)
    buf = (b"\xaa" if which == "a" else b"\xbb") * BLK
    off0 = 0 if which == "a" else BLK
    try:
        for i in range(N):
            os.pwrite(fd, buf, i * STRIDE + off0)
            os.fsync(fd)
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
    print("WRITE_%s n=%d done" % (which.upper(), N))


def verify(mnt):
    p = path(mnt)
    fd = os.open(p, os.O_RDONLY)
    lost_a = lost_b = decode = 0
    sample_a = sample_b = None
    try:
        for i in range(N):
            try:
                aa = os.pread(fd, BLK, i * STRIDE)
                bb = os.pread(fd, BLK, i * STRIDE + BLK)
            except OSError as e:
                decode += 1
                if sample_a is None:
                    sample_a = (i, "EIO %s" % e)
                continue
            if aa != b"\xaa" * BLK:
                lost_a += 1
                if sample_a is None:
                    sample_a = (i, aa[:16])
            if bb != b"\xbb" * BLK:
                lost_b += 1
                if sample_b is None:
                    sample_b = (i, bb[:16])
    finally:
        os.close(fd)
    lost = lost_a + lost_b
    print("VERIFY n=%d lost_a=%d lost_b=%d lost=%d decode_eio=%d" %
          (N, lost_a, lost_b, lost, decode))
    if sample_a:
        print("SAMPLE_A i=%s bytes=%r" % (sample_a[0], sample_a[1]))
    if sample_b:
        print("SAMPLE_B i=%s bytes=%r" % (sample_b[0], sample_b[1]))
    if lost or decode:
        sys.exit(1)


def main():
    if len(sys.argv) < 3:
        print("usage: n1_shared_pwrite.py prepare|write-a|write-b|verify <mnt>",
              file=sys.stderr)
        sys.exit(2)
    role, mnt = sys.argv[1], sys.argv[2]
    if role == "prepare":
        prepare(mnt)
    elif role == "write-a":
        write_side(mnt, "a")
    elif role == "write-b":
        write_side(mnt, "b")
    elif role == "verify":
        verify(mnt)
    else:
        print("bad role %s" % role, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
