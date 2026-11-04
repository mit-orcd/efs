#!/usr/bin/env python3
"""Two real clients writing one shared file in the ior hard layout (K38/K40, O1).

Usage: orcd_mini_shared_chunk.py write|verify FILE --writers W --ranks A-B [--records N]

Record k of writer r is 47008 bytes at offset (k*W + r)*47008, so most 128 KiB
chunks hold spans of two or three writers. Run `write` with disjoint rank
ranges on node9900 (RDMA client B) and node9901 (client A) against the same
file at the same time (one thread per rank, pwrite, fsync, close); then run
`verify --ranks 0-(W-1)` through both mounts: every byte must match. Exit 1 on
any write error or mismatch; the first failures are printed.
"""
import argparse, hashlib, os, sys, threading

REC = 47008


def record(r, k):
    seed = hashlib.sha256(('%d/%d' % (r, k)).encode()).digest()
    return (seed * (REC // len(seed) + 1))[:REC]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=('write', 'verify'))
    ap.add_argument('file')
    ap.add_argument('--writers', type=int, default=8)
    ap.add_argument('--ranks', required=True)
    ap.add_argument('--records', type=int, default=2000)
    a = ap.parse_args()
    lo, hi = map(int, a.ranks.split('-'))
    errors = []

    def writer(r):
        try:
            fd = os.open(a.file, os.O_WRONLY | os.O_CREAT, 0o644)
            for k in range(a.records):
                if os.pwrite(fd, record(r, k), (k * a.writers + r) * REC) != REC:
                    raise OSError('short write')
            os.fsync(fd)
            os.close(fd)
        except OSError as e:
            errors.append('rank %d: %s' % (r, e))

    if a.mode == 'write':
        ts = [threading.Thread(target=writer, args=(r,)) for r in range(lo, hi + 1)]
        [t.start() for t in ts]
        [t.join() for t in ts]
    else:
        fd = os.open(a.file, os.O_RDONLY)
        for k in range(a.records):
            for r in range(lo, hi + 1):
                got = os.pread(fd, REC, (k * a.writers + r) * REC)
                if got != record(r, k):
                    errors.append('record r=%d k=%d: %d bytes, %s' % (
                        r, k, len(got), 'zeros' if got == bytes(len(got)) else 'differs'))
        os.close(fd)
    print('%s %s ranks %s: %d errors' % (a.mode, a.file, a.ranks, len(errors)))
    for e in errors[:10]:
        print(' ', e)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
