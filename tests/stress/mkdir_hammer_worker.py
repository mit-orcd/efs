#!/usr/bin/env python3
"""Time a tight mkdir loop. One process, one private directory, unique names.

argv: parent procs seconds host
Each child creates parent/<host>-<proc>/ and mkdirs n000000, n000001, ...
until the deadline, records every latency, then removes what it created.
"""
import errno
import os
import sys
import time

parent, procs, secs, host = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    i = min(len(xs) - 1, int(round(p / 100.0 * (len(xs) - 1))))
    return xs[i] * 1000.0


def run(p):
    d = os.path.join(parent, "%s-%d" % (host, p))
    os.mkdir(d)
    lats = []
    errs = 0
    deadline = time.monotonic() + secs
    i = 0
    try:
        while time.monotonic() < deadline:
            name = os.path.join(d, "n%06d" % i)
            t0 = time.monotonic()
            try:
                os.mkdir(name)
            except OSError as e:
                errs += 1
                print(
                    "ERR %s p%d i%d mkdir %s"
                    % (host, p, i, errno.errorcode.get(e.errno, e.errno)),
                    flush=True,
                )
                if errs >= 8:
                    break
            else:
                lats.append(time.monotonic() - t0)
            i += 1
    finally:
        for name in os.listdir(d):
            try:
                os.rmdir(os.path.join(d, name))
            except OSError:
                pass
        try:
            os.rmdir(d)
        except OSError as e:
            print(
                "ERR %s p%d rmdir-own %s"
                % (host, p, errno.errorcode.get(e.errno, e.errno)),
                flush=True,
            )
    n = len(lats)
    print(
        "DONE %s p%d n=%d err=%d p50=%.2f p90=%.2f p99=%.2f max=%.2f sum=%.3f"
        % (
            host,
            p,
            n,
            errs,
            pct(lats, 50),
            pct(lats, 90),
            pct(lats, 99),
            (max(lats) * 1000.0 if lats else 0.0),
            sum(lats),
        ),
        flush=True,
    )
    return errs


kids = []
for i in range(procs):
    pid = os.fork()
    if pid == 0:
        try:
            os._exit(1 if run(i) else 0)
        except OSError as e:
            print(
                "ERR %s p%d setup %s" % (host, i, errno.errorcode.get(e.errno, e.errno)),
                flush=True,
            )
            os._exit(1)
    kids.append(pid)
bad = 0
for pid in kids:
    _, st = os.waitpid(pid, 0)
    bad += 1 if os.waitstatus_to_exitcode(st) else 0
print("HOST_DONE %s bad_procs=%d" % (host, bad), flush=True)
sys.exit(1 if bad else 0)
