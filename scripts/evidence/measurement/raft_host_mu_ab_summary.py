#!/usr/bin/env python3
"""Summarize raft_host_mu_ab.sh output (all runs concatenated) per arm.

Usage: raft_host_mu_ab_summary.py RESULTS.txt
Arms are the run labels without their trailing round number (base1 -> base).
Prints, per run and as per-arm means: creates/s, efsd CPU per create, loaded
probe create/stat/unlink p50/p95/p99, the leader's lock-wait share of its
context switches and its top lock-wait sites, and write-phase-prof means and
quantiles for every phase.
"""
import re, sys, collections, statistics as st

runs = []
cur = None
for line in open(sys.argv[1], errors='replace'):
    line = line.rstrip('\n')
    m = re.match(r'== (\S+) ', line)
    if m:
        cur = {'label': m.group(1), 'probe': {}, 'phase': {}, 'sites': []}
        cur['arm'] = re.sub(r'\d+$', '', cur['label'])
        runs.append(cur); section = None; continue
    if cur is None:
        continue
    if line.startswith('-- loaded'):
        section = 'loaded'; continue
    if line.startswith('-- idle'):
        section = 'idle'; continue
    m = re.match(r'\s+(create|stat|lookup_neg|unlink)\s+p50=\s*([\d.]+) p95=\s*([\d.]+) p99=\s*([\d.]+)', line)
    if m and section == 'loaded':
        cur['probe'][m.group(1)] = tuple(float(x) for x in m.group(2, 3, 4))
    m = re.search(r'= (\d+) files/s', line)
    if m: cur['rate'] = int(m.group(1))
    m = re.search(r'per create: all ([\d.]+) us, leader ([\d.]+) us', line)
    if m: cur['cpu'] = (float(m.group(1)), float(m.group(2)))
    m = re.match(r'\s+(\w+)=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)$', line)
    if m and m.group(1) not in ('mono_us', 'arc_fallback'):
        n, s, mx, p50, p95, p99 = map(int, m.group(2, 3, 4, 5, 6, 7))
        cur['phase'][m.group(1)] = (n, s / max(n, 1), p50, p95, p99, mx)
    m = re.match(r'\s+arc_fallback=(\d+)', line)
    if m: cur['arc_fallback'] = int(m.group(1))
    m = re.search(r'leader switches (\d+), lock-wait (\d+) \(([\d.]+)%\)', line)
    if m: cur['lock'] = (int(m.group(1)), int(m.group(2)), float(m.group(3)))
    m = re.match(r'\s+([\d.]+)% of lock waits\s+(.*)', line)
    if m: cur['sites'].append((float(m.group(1)), m.group(2)))

arms = list(dict.fromkeys(r['arm'] for r in runs))
def mean(xs):
    xs = [x for x in xs if x is not None]
    return st.mean(xs) if xs else float('nan')

print('## Runs')
print('| Run | creates/s | CPU/create all, leader (us) | loaded create p50/p95/p99 ms | stat p50/p95/p99 | unlink p50/p95/p99 | leader lock-wait % of switches |')
print('| --- | --- | --- | --- | --- | --- | --- |')
for r in runs:
    pr = r['probe']
    f = lambda k: '/'.join('%.2f' % x for x in pr[k]) if k in pr else '-'
    print('| %s | %s | %s | %s | %s | %s | %s |' % (
        r['label'], r.get('rate', '-'),
        '%.0f, %.0f' % r['cpu'] if 'cpu' in r else '-',
        f('create'), f('stat'), f('unlink'),
        '%.1f%% (%d of %d)' % (r['lock'][2], r['lock'][1], r['lock'][0]) if 'lock' in r else '-'))

print('\n## Per-arm means')
print('| Arm | runs | creates/s | CPU/create all (us) | CPU/create leader (us) | create p50/p95/p99 | stat p50/p95/p99 | leader lock-wait % |')
print('| --- | --- | --- | --- | --- | --- | --- | --- |')
for a in arms:
    rs = [r for r in runs if r['arm'] == a]
    def q(k, i): return mean([r['probe'][k][i] if k in r['probe'] else None for r in rs])
    print('| %s | %d | %.0f | %.0f | %.0f | %.2f/%.2f/%.2f | %.2f/%.2f/%.2f | %.1f%% |' % (
        a, len(rs), mean([r.get('rate') for r in rs]),
        mean([r['cpu'][0] if 'cpu' in r else None for r in rs]),
        mean([r['cpu'][1] if 'cpu' in r else None for r in rs]),
        q('create', 0), q('create', 1), q('create', 2), q('stat', 0), q('stat', 1), q('stat', 2),
        mean([r['lock'][2] if 'lock' in r else None for r in rs])))

print('\n## Leader write-phase-prof, per-arm mean of each run (mean us; p50/p95/p99 bucket bounds)')
phases = list(dict.fromkeys(p for r in runs for p in r['phase']))
print('| Phase | ' + ' | '.join(arms) + ' |')
print('| --- |' + ' --- |' * len(arms))
for p in phases:
    cells = []
    for a in arms:
        rs = [r['phase'][p] for r in runs if r['arm'] == a and p in r['phase']]
        if not rs:
            cells.append('-'); continue
        cells.append('%.1f (%s)' % (mean([x[1] for x in rs]),
                     '/'.join(str(int(round(mean([x[i] for x in rs])))) for i in (2, 3, 4))))
    print('| %s | %s |' % (p, ' | '.join(cells)))
print('\narc_fallback: ' + ', '.join('%s=%s' % (r['label'], r.get('arc_fallback', '-')) for r in runs))

print('\n## Leader lock-wait sites (share of lock waits, mean over runs; top 8 per arm)')
for a in arms:
    agg = collections.defaultdict(list)
    rs = [r for r in runs if r['arm'] == a]
    for r in rs:
        for share, site in r['sites']:
            agg[site].append(share)
    print('- %s:' % a)
    for site, v in sorted(agg.items(), key=lambda kv: -sum(kv[1]))[:8]:
        print('  - %.1f%% %s' % (sum(v) / len(rs), site))
