#!/usr/bin/env python3
"""Create-latency probe for the g_server->lock partition.

Two modes:
  probe <dir> <n> [write]  — time n creates (open O_CREAT + optional 1-byte
                             write + close), print latency stats in ms.
                             With `write`, each create also does a 1-byte
                             write so the close triggers a REPORT_CHUNKS
                             flush (the full create path, not just CREATE).
  burst <dir> <tag>        — create files in a tight loop forever (load gen).

Run `probe` on one client while `burst` runs on the others to measure the
create latency under a create burst. Compare vs the ~100-350ms pre-partition
burst latency (Aug 27 profiling) and the ~5-13ms rest latency.
"""
import os
import sys
import time


def probe(d, n, do_write):
    os.makedirs(d, exist_ok=True)
    lat = []
    for i in range(n):
        p = os.path.join(d, "probe-%d" % i)
        t0 = time.monotonic_ns()
        fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o644)
        if do_write:
            os.write(fd, b"x")
        os.close(fd)
        t1 = time.monotonic_ns()
        lat.append((t1 - t0) / 1e6)
    lat.sort()
    tag = "create+write+close" if do_write else "create(touch)"
    print("%s n=%d min=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f ms" % (
        tag, n, lat[0], lat[n // 2], lat[int(n * 0.90)], lat[int(n * 0.99)],
        lat[-1]))


def burst(d, tag):
    os.makedirs(d, exist_ok=True)
    i = 0
    pid = os.getpid()
    while True:
        fd = os.open(os.path.join(d, "burst-%s-%d-%d" % (tag, pid, i)),
                     os.O_CREAT | os.O_WRONLY, 0o644)
        os.write(fd, b"x")
        os.close(fd)
        i += 1


if __name__ == "__main__":
    if sys.argv[1] == "probe":
        probe(sys.argv[2], int(sys.argv[3]), len(sys.argv) > 4)
    elif sys.argv[1] == "burst":
        burst(sys.argv[2], sys.argv[3])
