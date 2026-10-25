#!/usr/bin/env python3
"""A11 finding 2: one table row per tests/live/append_failover.py run.

Usage: python3 scripts/report_stepdown_summary.py [--bound S] RUN_DIR...

RUN_DIR holds the harness's report.json and its logs/ (efsd-n1..3.log,
fuse-mnt.log). Per run: verdict and integrity, REPORT receive retries,
exhausted 8 s REPORT budgets, the slowest successful REPORT (the client
logs `slow-ok` from 1 s), the REPORTs over the A11 bound (S seconds,
default 3, per leader change crossed: S x max(1, n), n = harness events
from S before the REPORT started to its end; the rule of
tests/live/append_failover.py), recovery-scan resolutions (`txn-recover ... ->`:
a transaction nothing finished for 5 s), finisher hand-offs of a DECIDE
lost to a step-down (`txn-commit ... (finisher abort)`, `txn-drop`, `no
decision after`), the warm view's record counts and the step-downs that
took effect. Reads logs directly, so it also covers reports from harness
builds without the REPORT latency checks. Read-only.
"""
import calendar, json, re, sys, time
from pathlib import Path


def count(paths, rx):
    n, first = 0, []
    for p in paths:
        try:
            text = p.read_text(errors='replace')
        except FileNotFoundError:
            continue
        for line in text.splitlines():
            if re.search(rx, line):
                n += 1
                if len(first) < 3:
                    first.append(line[:200])
    return n, first


def row(d, bound):
    r = json.loads((d / 'report.json').read_text())
    c, w = r['checks'], r['checks'].get('warm_primary', {})
    fuse = [d / 'logs' / 'fuse-mnt.log']
    efsd = sorted((d / 'logs').glob('efsd-n*.log'))
    slow, over, worst = [], 0, 0.0
    events = [e['t'] for e in r.get('events', [])]
    for p in fuse:
        if not p.exists():
            continue
        for stamp, us in re.findall(r'^(\S+)Z inode-rpc: slow-ok type=67 .*us=(\d+)',
                                    p.read_text(errors='replace'), re.M):
            end = calendar.timegm(time.strptime(stamp[:19], '%Y-%m-%dT%H:%M:%S')) + float('0' + stamp[19:])
            us = int(us)
            n = sum(1 for t in events if end - us / 1e6 - bound <= t <= end)
            slow.append(us)
            over += us > bound * 1e6 * max(1, n)
            worst = max(worst, us / 1e6 / max(1, n))
    return dict(run=d.name, verdict=r['verdict'], integrity=r.get('integrity'),
                recv67=count(fuse, r'inode-rpc: retry type=67 .*why=recv')[0],
                budget=count(fuse, r'report-loop ')[0],
                report_max_ms=max(slow, default=0) // 1000, report_slow_ok=len(slow),
                max_s_per_change=round(worst, 2), over_bound=over,
                txn_recover=count(efsd, r'txn-recover .*-> ')[0],
                handoffs=count(efsd, r'\(finisher abort\)|no decision after')[0],
                visible=w.get('visible_records'), missing=w.get('missing'), dup=w.get('duplicates'),
                views_identical=c.get('views_identical'),
                stepdowns=sum(1 for e in r.get('events', []) if e.get('rc') == 0),
                fsync_max_s=round(c['final_fsync_max_s'], 2), wall_s=round(r.get('wall_seconds', 0)))


def main(argv):
    bound = 3.0
    if argv[:1] == ['--bound']:
        bound, argv = float(argv[1]), argv[2:]
    if not argv:
        sys.exit(__doc__)
    rows = [row(Path(a), bound) for a in argv]
    keys = list(rows[0])
    print('| ' + ' | '.join(keys) + ' |')
    print('| ' + ' | '.join('---' for _ in keys) + ' |')
    for x in rows:
        print('| ' + ' | '.join(str(x[k]) for k in keys) + ' |')


if __name__ == '__main__':
    main(sys.argv[1:])
