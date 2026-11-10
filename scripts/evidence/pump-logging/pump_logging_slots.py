#!/usr/bin/env python3
"""Task FD: summarize pump_logging_fsync_breakdown.sh output.

Usage: pump_logging_slots.py DIR   (DIR holds strace-n1..3.txt and fixture/result.json)
The W159 interposer holds one flock per process while it sleeps 40 ms, so the
three daemons take turns: one 40 ms slot per daemon every ~120 ms, threads of
one daemon sharing a slot. Prints per daemon the fsync-class calls, the slots
(calls closer than 20 ms), what each slot synced, the slot period, and with
the writes completed in the strace window the slots per write.
"""
import collections
import json
import re
import sys
from pathlib import Path


def sec(t):
    h, m, s = t.split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


d = Path(sys.argv[1])
res = json.loads((d / 'fixture/result.json').read_text())
spans, rows = [], []
for n in (1, 2, 3):
    ev, calls = [], collections.Counter()
    for line in (d / ('strace-n%d.txt' % n)).read_text().splitlines():
        m = re.match(r'\d+ (\d\d:\d\d:\d\d\.\d+) (fsync|fdatasync|sync_file_range)\(\d+(?:<([^>]*)>)?', line)
        if not m:
            continue
        t, sc, path = m.groups()
        kind = 'raft' if (path or '').endswith('raft.log') else 'kvwal' if (path or '').endswith('kv/wal') else 'bg'
        calls[kind] += 1
        if sc != 'sync_file_range':          # the interposer does not delay sync_file_range
            ev.append((sec(t), kind))
    ev.sort()
    slots = []
    for t, k in ev:
        if slots and t - slots[-1][0] < 0.02:
            slots[-1][1].add(k)
        else:
            slots.append([t, {k}])
    gaps = sorted(b[0] - a[0] for a, b in zip(slots, slots[1:]))
    span = slots[-1][0] - slots[0][0]
    spans.append(span)
    withraft = sum('raft' in s[1] for s in slots)
    rows.append((n, sum(calls.values()), len(slots), withraft, span, gaps[len(gaps) // 2] * 1000, dict(calls)))
# writes completed in the window: strace attaches ~4.2 s after the write window opens
wc = res['write_completions']
lo = 4.2
w = sum(1 for t, _ in wc if lo <= t <= lo + min(spans))
print('writes in the strace window (t=%.1f..%.1f s): %d' % (lo, lo + min(spans), w))
for n, c, s, r, span, gap, kinds in rows:
    print('n%d: %d fsync-class calls %s; %d slots in %.1f s (period %.1f ms), raft.log in %d slots; per write: %.1f calls, %.1f slots'
          % (n, c, kinds, s, span, gap, r, c / w, s / w))
print('writes %d p50 %.3f p99 %.3f' % (res['writes'], res['write_p50_s'], res['write_p99_s']))
