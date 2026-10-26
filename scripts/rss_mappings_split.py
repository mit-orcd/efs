#!/usr/bin/env python3
"""Growth per efsd over minutes [A, B) of a D4 load from a
rss_mappings_sample.py series, split by component (F1; see
docs/status/efsd-rss-plateau-20261025.md).

Usage: rss_mappings_split.py SERIES.jsonl LOAD_START_EPOCH_S A B [RING_KIB]

Each component's growth is its least-squares slope over the window times
the window's span (the scripts/rss_plateau.py metric), in MiB: RSS (and
as a share of the process's peak), the mapping of RING_KIB KiB (the
reclaim journal ring; 30732 before cbfbadbff, where it shared a VMA with
the fence tables, 24576 after), the other large anonymous mappings,
thread stacks, and the rest (file-backed and small anonymous).
LOAD_START_EPOCH_S: the mtime of d4-samples.json minus its last `t`."""
import json, sys
recs = [json.loads(l) for l in open(sys.argv[1])]
t0, a, b = float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
ring = sys.argv[5] if len(sys.argv) > 5 else '30732'


def slope(xs, ys):
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)


keys = sorted({k for r in recs for k in r['p'] if k.startswith('efsd')})
print('node  peak_MiB  dRSS  dring  dbig_other  dstacks  drest   (MiB over the window; % of peak for RSS)')
for k in keys:
    pts = [((r['t'] - t0) / 60, r['p'][k]) for r in recs if k in r['p'] and a <= (r['t'] - t0) / 60 < b]
    peak = max(r['p'][k]['rss'] for r in recs if k in r['p']) / 1024
    xs = [m for m, _ in pts]
    span = b - a
    g = lambda f: slope(xs, [f(p) / 1024 for _, p in pts]) * span
    rss = g(lambda p: p['rss']); rg = g(lambda p: p['sizes'].get(ring, 0))
    big = g(lambda p: p['big_rss'] - p['sizes'].get(ring, 0)); st = g(lambda p: p['stack_rss'])
    print(f"{k.split('/')[-1]:4s} {peak:8.1f} {rss:5.1f} ({100 * rss / peak:+.1f} %) {rg:5.1f} {big:8.1f} {st:8.1f} {rss - rg - big - st:6.1f}")
