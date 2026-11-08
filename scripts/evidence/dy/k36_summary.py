#!/usr/bin/env python3
"""Per-step wall times of k36_loop.sh output dirs, and the rebuild waits
their logs show. Usage: k36_summary.py OUT [OUT...] (one table each)."""
import re, statistics, sys
from pathlib import Path

CAUSES = {
    'W154 PUT/COMMIT refused (rc=-1)': r'raft-host: rebuild .* PUT failed \(rc=-1\)',
    'W154 PUT no answer (rc=-6)': r'raft-host: rebuild .* PUT failed \(rc=-6\)',
    'W154 sources unavailable, 10 s skip': r'raft-host: rebuild .* source fragments unavailable',
    'W154 COMMIT refused, fenced (no skip)': r'COMMIT refused \(store fenced\)',
    'K37 node skipped 10 s': r'segment-rebuild: node \d+ skipped for 10000 ms',
    'K37 node skipped < 10 s (cost-proportional)': r'segment-rebuild: node \d+ skipped for (?!10000 )\d+ ms',
    'W154 PUT failed, skipped < 10 s': r'PUT failed \(rc=-?\d+\); mark kept, node skipped (?!10000 )\d+ ms',
    'K37 sweep unproven (30 s retry)': r'sweep had unproven answers',
}


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]


def summary(out):
    steps, totals, causes, runs = {}, [], {k: 0 for k in CAUSES}, 0
    for o in sorted(Path(out).glob('run*.out')):
        if not o.with_suffix('.rc').exists():
            continue  # still running
        text = o.read_text(errors='replace')
        m = re.search(r'PASS \(([\d.]+) s\)\s*$', text)
        if not m:
            print(f'{o.name}: no PASS line (rc {o.with_suffix(".rc").read_text().strip()})')
            continue
        runs += 1
        totals.append(float(m.group(1)))
        for n, dt in re.findall(r'^(\d+)\. .*\[([\d.]+) s, at [\d.]+ s\]$', text, re.M):
            steps.setdefault(int(n), []).append(float(dt))
        logs = o.with_name(o.stem + '-logs')
        text = '\n'.join(p.read_text(errors='replace') for p in logs.rglob('n*.log')) \
            if logs.is_dir() else ''
        for k, pat in CAUSES.items():
            causes[k] += len(re.findall(pat, text))
    print(f'{out}: {runs} runs')
    print('step  median  p95    max    runs>5s')
    for n in sorted(steps):
        xs = steps[n]
        print(f'{n:>4}  {statistics.median(xs):6.1f} {pct(xs, 95):6.1f} {max(xs):6.1f}  '
              f'{sum(x > 5 for x in xs)}/{len(xs)}')
    if totals:
        print(f'total {statistics.median(totals):6.1f} {pct(totals, 95):6.1f} {max(totals):6.1f}')
    for k, v in causes.items():
        print(f'  {k}: {v}')


for d in sys.argv[1:]:
    summary(d)
