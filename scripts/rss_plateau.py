#!/usr/bin/env python3
"""RSS plateau check for the F1 memory gate (alpha 1.0 checklist F1).

Input: the `ps` sample file a loopback D4 run writes beside the driver
(one line per sample: `<epoch> <pid>:<rss_kib> ...`), or the driver's own
`d4-samples.json` (a list of samples, or a document with a `samples` list,
each with `client_rss_kib` per mount and `nodes.*.rss_kib`). For every
process it prints first, peak and last RSS, the growth over the last third
of the run and the verdict: PLATEAU when the last third grows by at most
--slope-pct of the peak (default 5 %), else GROWING. The growth is the
least-squares slope over the last third times its span: a single sample at
either end does not decide it (efsd's RSS swings by a few MiB from sample to
sample when its allocator returns pages). The endpoint difference is printed
beside it.

    python3 scripts/rss_plateau.py ps-samples.txt [--load-s 1200] [--slope-pct 5]

Exit 0 PLATEAU for every process, 1 otherwise, 2 on bad input. Samples
after --load-s seconds (the drain and verify phases) are not counted in the
slope: RSS may still rise there with the read-verify cache and fall after.
`evaluate()` is the same verdict for scripts/d4_progress.py (--plateau-pct)
and tests/test_d4_plateau.py.
"""
import argparse, json, sys
from collections import OrderedDict
from pathlib import Path


def from_ps(path):
    series = OrderedDict()
    t0 = None
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        try:
            t = int(parts[0])
        except ValueError:
            continue
        t0 = t if t0 is None else t0
        for item in parts[1:]:
            pid, kib = item.split(':')
            series.setdefault(pid, []).append((t - t0, int(kib)))
    return series


def from_samples(samples):
    """{name: [(seconds, kib), ...]} from the D4 driver's samples: one
    series per efs-fuse (`efs-fuse:<mount>`) and per efsd (`efsd:<node>`)."""
    series = OrderedDict()
    t0 = None
    for s in samples:
        t = s.get('t') or s.get('elapsed_s') or 0
        t0 = t if t0 is None else t0
        for m, kib in (s.get('client_rss_kib') or {}).items():
            if kib is not None:
                series.setdefault('efs-fuse:' + str(m), []).append((t - t0, kib))
        for n, b in (s.get('nodes') or {}).items():
            kib = (b or {}).get('rss_kib')
            if kib is not None:
                series.setdefault('efsd:' + str(n), []).append((t - t0, kib))
    return series


def space_from_samples(samples):
    """{'data:<node>': [(seconds, kib), ...]}: each node's quota usage
    (`efs-mgmt gc-status` data_usage_bytes) from the D4 driver's samples, in
    KiB so evaluate() reads it like RSS."""
    series = OrderedDict()
    t0 = None
    for s in samples:
        t = s.get('t') or s.get('elapsed_s') or 0
        t0 = t if t0 is None else t0
        for n, b in (s.get('nodes') or {}).items():
            v = (b or {}).get('data_bytes')
            if v is not None:
                series.setdefault('data:' + str(n), []).append((t - t0, v // 1024))
    return series


def from_json(path):
    doc = json.loads(Path(path).read_text())
    return from_samples(doc if isinstance(doc, list) else doc.get('samples', []))


def evaluate(series, load_s=None, slope_pct=5.0):
    """One row per process: first, peak and last RSS (MiB), the RSS at the
    start of the last third, the growth over the last third as a share of
    the peak, and the verdict (PLATEAU, GROWING, or 'too few' under four
    samples). Only samples up to load_s seconds count when it is given."""
    rows = []
    for name, pts in series.items():
        if load_s is not None:
            pts = [x for x in pts if x[0] <= load_s] or pts
        if len(pts) < 4:
            rows.append({'process': name, 'samples': len(pts), 'verdict': 'too few'})
            continue
        kib = [k for _, k in pts]
        third = pts[len(pts) * 2 // 3:]
        n = len(third)
        mt = sum(t for t, _ in third) / n
        mk = sum(k for _, k in third) / n
        var = sum((t - mt) ** 2 for t, _ in third)
        slope = sum((t - mt) * (k - mk) for t, k in third) / var if var else 0.0
        growth = slope * (third[-1][0] - third[0][0])
        peak = max(kib)
        pct = 100.0 * growth / peak if peak else 0.0
        end_pct = 100.0 * (third[-1][1] - third[0][1]) / peak if peak else 0.0
        rows.append({'process': name, 'first_mib': round(kib[0] / 1024, 1), 'peak_mib': round(peak / 1024, 1),
                     'last_mib': round(kib[-1] / 1024, 1), 'last_third_start_mib': round(third[0][1] / 1024, 1),
                     'growth_pct': round(pct, 2), 'endpoint_pct': round(end_pct, 2),
                     'verdict': 'PLATEAU' if pct <= slope_pct else 'GROWING',
                     'samples': len(pts), 'span_s': pts[-1][0] - pts[0][0]})
    return rows


def evaluate_peaks(series, load_s=None, peak_pct=25.0):
    """Bounded-sawtooth rule for stored data, one row per node: the peak of
    the last third of the load against the peak before it. Reclaim works in
    batches, so the series rises and falls; a least-squares slope over the
    last third reads a falling half-tooth as growth (+8.5 % on a bounded
    60-minute run), while a store that keeps what it should reclaim raises
    every later peak (+35-44 % over 8 minutes). It detects growth that is
    large against the live data over the run, not a slow drift.
    growth_pct is the last third's peak over the earlier peak; PLATEAU
    (bounded) at most peak_pct."""
    rows = []
    for name, pts in series.items():
        if load_s is not None:
            pts = [x for x in pts if x[0] <= load_s] or pts
        if len(pts) < 6:
            rows.append({'process': name, 'samples': len(pts), 'verdict': 'too few'})
            continue
        k = len(pts) * 2 // 3
        before, last = max(v for _, v in pts[:k]), max(v for _, v in pts[k:])
        pct = 100.0 * (last - before) / before if before else 0.0
        kib = [v for _, v in pts]
        rows.append({'process': name, 'first_mib': round(kib[0] / 1024, 1), 'peak_mib': round(max(kib) / 1024, 1),
                     'last_mib': round(kib[-1] / 1024, 1), 'last_third_start_mib': round(pts[k][1] / 1024, 1),
                     'growth_pct': round(pct, 2),
                     'endpoint_pct': round(100.0 * (kib[-1] - kib[0]) / max(kib), 2) if max(kib) else 0.0,
                     'verdict': 'PLATEAU' if pct <= peak_pct else 'GROWING',
                     'samples': len(pts), 'span_s': pts[-1][0] - pts[0][0]})
    return rows


def format_rows(rows):
    out = ['%-28s %9s %9s %9s %9s %9s %9s  %s' % ('process', 'first', 'peak', 'last', 'lastthird', 'growth', 'endpoint',
                                                    'verdict')]
    for r in rows:
        if r['verdict'] == 'too few':
            out.append('%-28s %d samples: too few' % (r['process'], r['samples']))
            continue
        out.append('%-28s %7.1fMi %7.1fMi %7.1fMi %7.1fMi %+7.1f%% %+7.1f%%  %s (%d samples over %ds)'
                   % (r['process'], r['first_mib'], r['peak_mib'], r['last_mib'], r['last_third_start_mib'],
                      r['growth_pct'], r['endpoint_pct'], r['verdict'], r['samples'], r['span_s']))
    return '\n'.join(out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('path')
    p.add_argument('--load-s', type=float, default=None, help='only samples up to this many seconds count')
    p.add_argument('--slope-pct', type=float, default=5.0)
    a = p.parse_args()
    series = from_json(a.path) if a.path.endswith('.json') else from_ps(a.path)
    if not series:
        sys.exit('no samples in %s' % a.path)
    rows = evaluate(series, a.load_s, a.slope_pct)
    print(format_rows(rows))
    rc = 0 if all(r['verdict'] == 'PLATEAU' for r in rows) else 1
    print('VERDICT:', 'PLATEAU' if rc == 0 else 'GROWING')
    return rc


if __name__ == '__main__':
    sys.exit(main())
