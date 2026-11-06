#!/usr/bin/env python3
"""K88 reach (Task DK): count `report-stuck` from slot-less efs-bench writers
of multi-chunk files under REPORT contention, on one private cluster.

Three private production efsd (tests/k33_daemons.py, TCP, free loopback
ports). ROUNDS rounds of PAR concurrent `efs-bench --size SIZE --io-size IO
--workers WORKERS --id N`; every 64 KiB write PUTs the chunk's whole image so
far and the 10 ms background REPORT commits half images while the writer is
mid-chunk (the K88 race). A writer's chunk loses its committed base when its
CAS hint is gone; that record then ends in `report-stuck` and `BENCH_FAIL`.

Run from a built tree (efsd, efs-mgmt, efs-bench):
  python3 scripts/evidence/k88/bench_chunk_hints.py OUTDIR [PAR=24] [ROUNDS=20]
      [SIZE=4M] [IO=64K] [WORKERS=4] [LEADERS=any]
LEADERS=split moves group 0's leader to node 1 and group 2's to node 2 before
the first round (the K96 cycle needs them apart); `any` keeps the election's.
The placement is printed in the summary either way.
Output: OUTDIR/runs.tsv (round copy rc report_stuck secs start_epoch),
OUTDIR/summary.txt; the stderr of every run with a `report-stuck` or a failure
is kept as OUTDIR/r<round>-<copy>.log and the daemons' logs as OUTDIR/n<i>.log.
The cluster's work directory is removed at the end.
"""
import os, re, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / 'tests'))
import k33_daemons as k

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
par, rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 24, int(sys.argv[3]) if len(sys.argv) > 3 else 20
size, io, workers = (sys.argv[4:7] + ['4M', '64K', '4'][len(sys.argv[4:7]):])
leaders = sys.argv[7] if len(sys.argv) > 7 else 'any'
assert leaders in ('any', 'split'), leaders
k.EFSD = ROOT / 'efsd'
env = {key: v for key, v in os.environ.items() if not key.startswith('EFS_')}
env['EFS_TRANSPORT'] = 'tcp'
work = Path(tempfile.mkdtemp(prefix='efs-k88b-', dir=os.environ.get('TMPDIR')))
c = k.Cluster(work, 'k88b', [1, 2, 3], env)


def one(r, i):
    t, start = time.monotonic(), time.time()
    p = subprocess.run([str(ROOT / 'efs-bench'), c.ep(1 + i % 3), '--size', size, '--io-size', io,
                        '--workers', workers, '--id', str(100000 + r * 1000 + i)],
                       env=env, text=True, capture_output=True, timeout=300)
    stuck = len(re.findall(r'^report-stuck ', p.stderr, re.M))
    if stuck or p.returncode:
        (out / f'r{r}-{i}.log').write_text(p.stdout + p.stderr)
    return r, i, p.returncode, stuck, time.monotonic() - t, start


try:
    for i in (1, 2, 3):
        c.start(i, join=None if i == 1 else 1, quota='8G')
    c.wait_members((1, 2, 3), 3)
    c.wait_voters((1, 2, 3), lambda n, v: k.voters_ok(n, v, 3))
    if leaders == 'split':
        k.place_leaders(c, 1, groups=(0,))
        k.place_leaders(c, 2, groups=(2,))
    placement = c.leader_placement(1)
    rows = []
    with ThreadPoolExecutor(par) as pool:
        for r in range(rounds):
            rows += list(pool.map(lambda i: one(r, i), range(par)))
    with open(out / 'runs.tsv', 'w') as f:
        f.writelines(f'{r}\t{i}\t{rc}\t{s}\t{secs:.2f}\t{t0:.3f}\n' for r, i, rc, s, secs, t0 in rows)
    text = (f'size {size} io {io} workers {workers} par {par} rounds {rounds} '
            f'leaders {leaders} g0=node{placement[0]} g2=node{placement[2]} '
            f'final g0=node{c.leader_placement(1)[0]} g2=node{c.leader_placement(1)[2]}\n'
            f'runs {len(rows)} fail {sum(1 for x in rows if x[2])} '
            f'runs_with_report_stuck {sum(1 for x in rows if x[3])} '
            f'report_stuck_lines {sum(x[3] for x in rows)}\n')
    (out / 'summary.txt').write_text(text)
    print(text, end='')
finally:
    c.stop_all()
    for i in (1, 2, 3):
        if (c.dir / f'n{i}.log').exists():
            shutil.copy(c.dir / f'n{i}.log', out / f'n{i}.log')
    shutil.rmtree(work, ignore_errors=True)
