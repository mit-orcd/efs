#!/usr/bin/env python3
"""K110 (Task FH): summarize `run_series.sh` output directories after the barrier proof.

usage: summarize_fix.py <WORK>/out [<WORK2>/out ...]
One row per run: rc, wall seconds, kills, verdict, the writers' final-fsync errnos,
the --post-fsync errnos, the client's "restarted members: N fragments proven by
their barrier history, M lost" counts (summed over its fsyncs), the stripes the
final fsyncs marked degraded, and the files verified recovery dropped. The raw
report, client log and member logs stay in <out>/<run>.*.
"""
import glob, json, os, re, sys

rows = []
for out in sys.argv[1:]:
    summary = open(os.path.join(out, 'summary.txt')).read()
    for rep in sorted(glob.glob(os.path.join(out, '*.report.json'))):
        run = os.path.basename(rep)[:-len('.report.json')]
        r = json.load(open(rep))
        rc = re.search(rf'run {re.escape(run)} rc=(\d+) wall=(\d+)s', summary)
        logs = os.path.join(out, run + '.logs')
        client = ''
        for name in ('fuse-mnt.log',):
            p = os.path.join(logs, name)
            if os.path.exists(p):
                client += open(p, errors='replace').read()
        hist = re.findall(r'restarted members: (\d+) fragments proven by their barrier history, (\d+) lost', client)
        proven = sum(int(a) for a, _ in hist)
        lost = sum(int(b) for _, b in hist)
        marked = len(re.findall(r'marked degraded for rebuild', client)) + sum(
            int(n) for n in re.findall(r'(\d+) more stripes marked degraded', client))
        dropped = 0
        for f in glob.glob(os.path.join(logs, 'efsd-n*.log')):
            dropped += sum(int(n) for n in re.findall(r'recovery: verified .*? dropped=(\d+)',
                                                      open(f, errors='replace').read()))
        fs = ''.join(str(w['final_fsync_errno']) for w in r.get('writers', []))
        post = ''.join(str(p['errno']) for p in r.get('post_recovery_fsync', []))
        rows.append((run, rc.group(1) if rc else '?', rc.group(2) if rc else '?', len(r.get('events', [])),
                     r.get('verdict'), fs, post, proven, lost, marked, dropped))
print('run\trc\twall_s\tkills\tverdict\tfinal_fsync_errnos\tpost_fsync_errnos\tproven_by_history\tlost\tmarked\trecovery_dropped')
for row in rows:
    print('\t'.join(str(x) for x in row))
tot = [sum(r[i] for r in rows) for i in (7, 8, 9, 10)]
print(f'TOTAL runs={len(rows)} pass={sum(r[4] == "PASS" for r in rows)} proven={tot[0]} lost={tot[1]} '
      f'marked={tot[2]} recovery_dropped={tot[3]}')
