#!/usr/bin/env python3
"""Per-run K110 table from the kept <out>/<run>.report.json and <out>/<run>.logs/ (Task FH).

usage: analyze_runs.py <out dir written by run_series.sh>   (one JSON object per run)
first_refusal_after_last_restart_s is meaningful only when down_at_fsync is empty."""
import json, re, sys, glob, os, calendar, time, collections
out = sys.argv[1]
def ts(s):  # 2026-11-10T06:51:22.474Z -> epoch
    return calendar.timegm(time.strptime(s[:19], '%Y-%m-%dT%H:%M:%S')) + float(s[19:23] or 0)
rx = re.compile(r'^(\S+) fsync ino=\d+ chunk=(\d+) gen=(\w+) nodes=(\d+),(\d+),(\d+) fragments=(\w{3}): (\d) of 3 durable; (.*) \(rc=(-?\d+)\)')
rows = []
for rep in sorted(glob.glob(out + '/*.report.json'), key=lambda p: (os.path.basename(p)[0], int(re.findall(r'-(\d+)\.', p)[0]))):
    name = os.path.basename(rep)[:-12]
    r = json.load(open(rep)); logs = f'{out}/{name}.logs'
    kills = [(e['t'], e.get('killed')) for e in r['events'] if e.get('killed')]
    # last restart per node = last "listening on" line in that node's log
    refused = collections.OrderedDict(); first = None; more = []
    for l in open(f'{logs}/fuse-mnt.log', errors='replace'):
        m = re.match(r'^(\S+) fsync: (\d+) more stripes without proof', l)
        if m: more.append(int(m[2]))
        m = rx.match(l)
        if not m: continue
        t = ts(m[1])
        if m[10] == '-1':
            first = first or t
            key = (m[2], m[3]); refused.setdefault(key, []).append((t, m[7], m[8], 'L' in m[7]))
    restart = {}
    for n in (1, 2, 3):
        t = None
        for l in open(f'{logs}/efsd-n{n}.log', errors='replace'):
            if 'listening on' in l and (first is None or ts(l.split()[0]) <= first): t = ts(l.split()[0])
        restart[n] = t
    pf0 = r.get('post_recovery_fsync', [])
    restart_post = {}
    for n in (1, 2, 3):
        t = None
        for l in open(f'{logs}/efsd-n{n}.log', errors='replace'):
            if 'listening on' in l and (not pf0 or ts(l.split()[0]) <= pf0[0]['t']): t = ts(l.split()[0])
        restart_post[n] = t
    resent = 0   # a stripe whose D count ever grows, or an L/M/P that returns to P after L: a re-acknowledgement
    for k, v in refused.items():
        for a, b in zip(v, v[1:]):
            if a[1].count('D') < b[1].count('D') or ('L' in a[1] and b[1].count('P') > a[1].count('P')): resent += 1
    changed = [k for k, v in refused.items() if len({x[1] for x in v}) > 1]
    pats = collections.Counter(v[0][1] for v in refused.values())
    pf = r.get('post_recovery_fsync', [])
    wr = r['writers']
    last_kill_t, last_kill_n = kills[-1] if kills else (None, None)
    last_restart = restart_post.get(last_kill_n)
    rows.append(dict(run=name, verdict=r['verdict'], integrity=r['checks'].get('warm_primary', {}) and 'exact' if r['checks'].get('cold', {}).get('acknowledged') == r['checks'].get('cold', {}).get('visible_records') else 'CHECK',
        final=[w['final_fsync_errno'] for w in wr], final_s=round(max(w['final_fsync_seconds'] for w in wr), 2),
        kills=' '.join(f'n{n}@{t - r["started"]:.0f}s' for t, n in kills),
        first_refusal_after_last_restart_s=None if not first or not restart.get(last_kill_n) else round(first - restart[last_kill_n], 1), resent_signs=resent,
        down_at_fsync=None if not first else [n for t, n in kills if t <= first <= t + 5.5],
        stripes=len(refused), more_without_proof=more[:1], patterns=dict(pats.most_common(6)), changed=len(changed), last_down=None if not first else (last_kill_n if last_kill_t <= first <= last_kill_t + 5.5 + 3 else None),
        post=[(p['label'], p['errno'], p['seconds'], round(p['t'] - last_restart, 1) if last_restart else None) for p in pf]))
for x in rows: print(json.dumps(x))
