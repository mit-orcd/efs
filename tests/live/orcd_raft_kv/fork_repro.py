#!/usr/bin/env python3
"""W121 kernel gate: direct-I/O reads racing fork must retain exact bytes.

Use a fresh directory on the mounted filesystem. Eight fixed 512-byte files
remain for independent verification. No application O_DIRECT flag is used;
EFS's FOPEN_DIRECT_IO selects the kernel path. Exit 1 means a wrong read.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import threading
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('path', type=Path)
p.add_argument('mode', choices=['fork', 'spawn', 'none'])
p.add_argument('--seconds', type=int, default=30)
a = p.parse_args()
a.path.mkdir(exist_ok=False)
for i in range(8):
    (a.path / str(i)).write_bytes(bytes([65 + i]) * 512)
end = time.monotonic() + a.seconds
results = []

def reader(i):
    n = bad = 0
    first = None
    with (a.path / str(i)).open('rb', buffering=0) as f:
        b = bytearray(512)
        while time.monotonic() < end:
            b[:] = b'?' * 512
            f.seek(0)
            got = f.readinto(b)
            n += 1
            if got != 512 or b != bytes([65 + i]) * 512:
                bad += 1
                if first is None:
                    first = dict(got=got, prefix=bytes(b[:32]).hex())
    results.append(dict(worker=i, reads=n, bad=bad, first=first))

def forkloop():
    n = 0
    while time.monotonic() < end:
        if a.mode == 'fork':
            pid = os.fork()
            if pid == 0:
                os._exit(0)
            os.waitpid(pid, 0)
        elif a.mode == 'spawn':
            pid = os.posix_spawn('/bin/true', ['/bin/true'], os.environ)
            os.waitpid(pid, 0)
        else:
            time.sleep(.001)
        n += 1
    results.append(dict(mode=a.mode, spawns=n))

threads = [threading.Thread(target=reader, args=(i,)) for i in range(8)]
threads.append(threading.Thread(target=forkloop))
for t in threads:
    t.start()
for t in threads:
    t.join()
reads = sum(r.get('reads', 0) for r in results)
bad = sum(r.get('bad', 0) for r in results)
print(json.dumps(dict(kernel=platform.release(), mode=a.mode, seconds=a.seconds,
                      reads=reads, failures=bad, workers=results)))
raise SystemExit(bool(bad) or len(results) != 9 or not reads)
