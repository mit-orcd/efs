#!/usr/bin/env python3
"""Reduce an strace -f -tt -T file: command, wall, per-syscall totals for
the efs mount, slow calls, errors. Usage: strace_reduce.py FILE [MOUNT]"""
import collections
import re
import sys

path = sys.argv[1]
mnt = sys.argv[2] if len(sys.argv) > 2 else "/tmp/efs-mount"
line_re = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+(.*)$')
dur_re = re.compile(r'<(\d+\.\d+)>\s*$')
name_re = re.compile(r'^(?:<\.\.\. )?(\w+)')
resumed_re = re.compile(r'^<\.\.\. (\w+) resumed>')
err_re = re.compile(r'= -1 (E[A-Z]+)')

first = last = None
cmd = None
tot = collections.Counter()
cnt = collections.Counter()
mx = collections.defaultdict(float)
errs = collections.Counter()
slow = []
all_cnt = collections.Counter()
all_tot = collections.Counter()
pending = {}  # (pid, name) -> line for unfinished
n = 0
for raw in open(path, errors='replace'):
    m = line_re.match(raw)
    if not m:
        continue
    pid, ts, rest = m.groups()
    if first is None:
        first = ts
    last = ts
    if cmd is None and rest.startswith('execve('):
        cmd = rest[:160]
    n += 1
    if rest.endswith('<unfinished ...>'):
        nmm = name_re.match(rest)
        if nmm:
            pending[(pid, nmm.group(1))] = rest
        continue
    rm = resumed_re.match(rest)
    if rm:
        nm = rm.group(1)
        head = pending.pop((pid, nm), '')
        rest = head.replace(' <unfinished ...>', '') + rest[rm.end():]
    else:
        nmm = name_re.match(rest)
        if not nmm:
            continue
        nm = nmm.group(1)
    dm = dur_re.search(rest)
    d = float(dm.group(1)) if dm else 0.0
    all_cnt[nm] += 1
    all_tot[nm] += d
    is_efs = mnt in rest
    if is_efs:
        cnt[nm] += 1
        tot[nm] += d
        if d > mx[nm]:
            mx[nm] = d
        em = err_re.search(rest)
        if em:
            errs[(nm, em.group(1))] += 1
    if d >= 0.2 and nm not in ('pselect6', 'select', 'poll', 'wait4', 'futex',
                               'clock_nanosleep', 'nanosleep', 'epoll_wait',
                               'read', 'ppoll', 'rt_sigtimedwait'):
        slow.append((d, ts, pid, rest[:200]))

def secs(ts):
    h, m_, s = ts.split(':')
    return int(h) * 3600 + int(m_) * 60 + float(s)

wall = secs(last) - secs(first) if first and last else 0
print("cmd:", cmd)
print("span: %s .. %s  wall=%.1fs  lines=%d" % (first, last, wall, n))
print("efs syscalls: %d  time in efs syscalls: %.2fs (%.0f%% of wall)" %
      (sum(cnt.values()), sum(tot.values()), 100 * sum(tot.values()) / wall if wall else 0))
print("%-14s %8s %10s %9s %9s" % ("efs syscall", "count", "total_s", "mean_ms", "max_ms"))
for nm, t in sorted(tot.items(), key=lambda kv: -kv[1])[:14]:
    print("%-14s %8d %10.3f %9.2f %9.1f" % (nm, cnt[nm], t, 1000 * t / cnt[nm], 1000 * mx[nm]))
if errs:
    print("efs errors:")
    for (nm, e), c in errs.most_common(12):
        print("  %-12s %-8s %d" % (nm, e, c))
print("slow (>= 0.2 s, work syscalls): %d" % len(slow))
for d, ts, pid, rest in sorted(slow, reverse=True)[:12]:
    print("  %8.3fs %s %s %s" % (d, ts, pid, rest))
