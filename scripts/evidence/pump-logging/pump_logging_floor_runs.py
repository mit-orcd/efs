#!/usr/bin/env python3
"""Task FD: run tests/test_pump_logging.py N times and keep each run's numbers.

Wraps profile.run() of the unchanged test: after every run it prints one
`FDRUN` line (loadavg, writes, p50, p99, writes per 5 s window) and appends
the result without samples/completions to --out. Usage, from a tree root:
  python3 scripts/evidence/pump-logging/pump_logging_floor_runs.py --runs 5 --out DIR [--test tests/test_pump_logging.py]
"""
import argparse
import collections
import json
import os
from pathlib import Path
import runpy
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'scripts/evidence/pump-logging'))
import pump_logging_profile as profile

p = argparse.ArgumentParser()
p.add_argument('--runs', type=int, default=5)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--test', default='tests/test_pump_logging.py')
p.add_argument('--tag', default='run')
p.add_argument('--skip-startup-refusal', action='store_true',
               help='unfixed trees accept an unsupported stderr sink; skip that step so the measured phases are reached')
args = p.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
orig = profile.run
state = {}


def run(a):
    state.setdefault('load', os.getloadavg())
    try:
        r = orig(a)
    except BaseException:
        state['failed'] = True
        rj = a.output / 'result.json'   # written by the profile's finally, phases included once the workload ended
        if a.sink == 'blocked' and a.seconds in (25, 35) and rj.exists():
            state['r'] = json.loads(rj.read_text())
        raise
    if a.sink == 'blocked' and a.seconds in (25, 35):   # the unmount-blocked phase (10 s, no fsync delay) is not measured
        state['r'] = r
    return r


profile.run = run
if args.skip_startup_refusal:
    profile.require_reporter_startup_failure = lambda *a, **k: None
for i in range(1, args.runs + 1):
    state.clear()
    t0 = time.time()
    rc, err = 0, ''
    try:
        runpy.run_path(str(ROOT / args.test), run_name='__main__')
    except SystemExit as e:
        rc = e.code or 0
    except BaseException as e:   # the test's own assertion: report, keep going
        rc, err = 1, repr(e)[:300]
    r = state.get('r', {})
    wc = r.get('write_completions', [])
    win = collections.Counter(int(t // 5) for t, _ in wc)
    row = dict(tag=args.tag, run=i, rc=rc, err=err, wall_s=round(time.time() - t0, 1),
               load_at_start=state.get('load'), load_at_end=os.getloadavg(),
               verdict=r.get('verdict'), writes=r.get('writes'), p50=r.get('write_p50_s'),
               p99=r.get('write_p99_s'), per_5s=[win.get(w, 0) for w in range(7)],
               phases=r.get('phases'), errors=len(r.get('errors') or []), late=r.get('late_writes'),
               stat_p50=r.get('stat_p50_s'), daemon_cpu_s=r.get('daemon_cpu_s'))
    print('FDRUN', json.dumps(row), flush=True)
    with (args.out / ('%s.jsonl' % args.tag)).open('a') as f:
        f.write(json.dumps(row) + '\n')
