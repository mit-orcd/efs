#!/usr/bin/env python3
"""RSS plateau check for the F1 memory gate (alpha 1.0 checklist F1).

Input: the `ps` sample file a loopback D4 run writes beside the driver
(one line per sample: `<epoch> <pid>:<rss_kib> ...`), or the driver's own
`d4_progress.json` (`samples[].client_rss_kib` / `nodes.*.rss_kib`). For
every process it prints first, peak and last RSS, the growth over the last
third of the run and the verdict: PLATEAU when the last third grows by at
most --slope-pct of the peak (default 5 %), else GROWING.

    python3 scripts/rss_plateau.py ps-samples.txt [--load-s 1200] [--slope-pct 5]

Exit 0 PLATEAU for every process, 1 otherwise, 2 on bad input. Samples
after --load-s seconds (the drain and verify phases) are not counted in the
slope: RSS may still rise there with the read-verify cache and fall after.
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


def from_json(path):
    doc = json.loads(Path(path).read_text())
    series = OrderedDict()
    t0 = None
    for s in doc.get('samples', []):
        t = s.get('t') or s.get('elapsed_s') or 0
        t0 = t if t0 is None else t0
        for m, kib in (s.get('client_rss_kib') or {}).items():
            if kib is not None:
                series.setdefault('client:' + m, []).append((t - t0, kib))
        for n, b in (s.get('nodes') or {}).items():
            kib = (b or {}).get('rss_kib')
            if kib is not None:
                series.setdefault('efsd:' + n, []).append((t - t0, kib))
    return series



def assess_points(pts, warmup_s=120, minimum_s=360):
    """Assess one uninterrupted write-load window; never bless short input.

    Skip two minutes of allocation/restart warmup and require six measured
    minutes, so the unchanged last-third 5% rule observes at least two minutes.
    Callers retain NOTRUN windows and require coverage separately.
    """
    if not pts:
        return dict(verdict='NOTRUN', reason='no samples')
    if any(k < 0 for _, k in pts) or any(b[0] <= a[0] for a,b in zip(pts,pts[1:])):
        return dict(verdict='FAIL', reason='invalid RSS sample order/value')
    selected = [(t,k) for t,k in pts if t >= pts[0][0]+warmup_s]
    if len(selected)<4 or selected[-1][0]-selected[0][0]<minimum_s:
        return dict(verdict='NOTRUN', reason='insufficient uninterrupted post-warmup write load',
                    samples=len(selected), required_seconds=minimum_s, warmup_seconds=warmup_s)
    values=[k for _,k in selected]
    third=selected[len(selected)*2//3:]
    peak=max(values)
    growth=100*(third[-1][1]-third[0][1])/peak if peak else 0
    return dict(verdict='PASS' if growth<=5 else 'FAIL', samples=len(selected),
                seconds=selected[-1][0]-selected[0][0], first=values[0], peak=peak,
                last=values[-1], lastthird=third[0][1], growth_pct=growth,
                warmup_seconds=warmup_s, threshold_pct=5)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('path')
    p.add_argument('--load-s', type=float, default=None, help='only samples up to this many seconds count')
    p.add_argument('--slope-pct', type=float, default=5.0)
    a = p.parse_args()
    series = from_json(a.path) if a.path.endswith('.json') else from_ps(a.path)
    if not series:
        sys.exit('no samples in %s' % a.path)
    rc = 0
    print('%-16s %9s %9s %9s %9s %9s  %s' % ('process', 'first', 'peak', 'last', 'lastthird', 'growth', 'verdict'))
    for name, pts in series.items():
        if a.load_s is not None:
            pts = [x for x in pts if x[0] <= a.load_s] or pts
        if len(pts) < 4:
            print('%-16s %d samples: too few' % (name, len(pts)))
            rc = 1
            continue
        kib = [k for _, k in pts]
        third = pts[len(pts) * 2 // 3:]
        growth = third[-1][1] - third[0][1]
        peak = max(kib)
        pct = 100.0 * growth / peak if peak else 0.0
        verdict = 'PLATEAU' if pct <= a.slope_pct else 'GROWING'
        if verdict != 'PLATEAU':
            rc = 1
        print('%-16s %7.1fMi %7.1fMi %7.1fMi %7.1fMi %+7.1f%%  %s (%d samples over %ds)'
              % (name, kib[0] / 1024, peak / 1024, kib[-1] / 1024, third[0][1] / 1024, pct, verdict,
                 len(pts), pts[-1][0] - pts[0][0]))
    print('VERDICT:', 'PLATEAU' if rc == 0 else 'GROWING')
    return rc


if __name__ == '__main__':
    sys.exit(main())
