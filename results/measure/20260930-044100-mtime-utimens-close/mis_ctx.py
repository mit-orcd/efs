#!/usr/bin/env python3
"""For each 'verification metadata mismatch' write(2) in an strace -f file,
print the preceding syscalls of that thread that name a path."""
import collections
import re
import sys

path = sys.argv[1]
hist = collections.defaultdict(list)
line_re = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+(.*)$')
n = 0
lens = collections.Counter()
for raw in open(path, errors='replace'):
    m = line_re.match(raw)
    if not m:
        continue
    pid, ts, rest = m.groups()
    if 'verification metadata mis' in rest and rest.startswith('write(2'):
        n += 1
        lm = re.search(r'"\.\.\., (\d+)', rest)
        if lm:
            lens[int(lm.group(1))] += 1
        if n <= 12:
            print("=== #%d pid %s %s" % (n, pid, ts))
            ctx = [l for l in hist[pid] if 'efs-mount' in l or '/data1/' in l][-4:]
            for l in ctx:
                print("   ", l[:230])
    h = hist[pid]
    h.append(ts + ' ' + rest)
    if len(h) > 40:
        del h[0]
print("total mismatch writes:", n)
print("message lengths:", sorted(lens.items()))
