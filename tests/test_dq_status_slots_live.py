#!/usr/bin/env python3
"""Task DQ on real daemons: forwarded ticket STATUS never holds the proof slots.

Three private production efsd (TCP, free loopback ports). ROUNDS rounds of 24
concurrent `efs-bench --size 8M --io-size 64K --workers 4`. Every fragment PUT
asks the ticket's lane leader for STATUS; the leader serving it needs a session
ReadIndex from the other group's leader. Before Task DQ the STATUS forward and
that ReadIndex shared the two peer proof slots, so with g0 and g2 led by
different nodes the forwards held the slots in both directions, every PUT on
the cluster answered BUSY after 400 ms, and the writers' BUSY retries kept the
cycle closed until their 30 s budget failed every copy with `rc=-13`.

Pass: every copy exits 0 and no worker reports `rc=-13`.
DQ_ROUNDS sets the rounds (default 3); DQ_KEEP=1 keeps the cluster directory.
"""
import os, re, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as k

ROOT = Path(__file__).resolve().parents[1]
PAR, ROUNDS = 24, int(os.environ.get('DQ_ROUNDS', '3'))
k.EFSD = ROOT / 'efsd'
env = {key: value for key, value in os.environ.items() if not key.startswith('EFS_')}
env['EFS_TRANSPORT'] = 'tcp'
work = Path(tempfile.mkdtemp(prefix='efs-dq-', dir=os.environ.get('TMPDIR')))
c = k.Cluster(work, 'dq', [1, 2, 3], env)
t0 = time.time()


def one(r, i):
    t = time.monotonic()
    p = subprocess.run([str(ROOT / 'efs-bench'), c.ep(1 + i % 3), '--size', '8M', '--io-size', '64K',
                        '--workers', '4', '--id', str(200000 + r * 1000 + i)],
                       env=env, text=True, capture_output=True, timeout=120 * k.SLOW)
    return i, p.returncode, len(re.findall(r'^bench worker=\d+ .*rc=-13$', p.stderr, re.M)), \
        time.monotonic() - t, p.stderr


try:
    for i in (1, 2, 3):
        c.start(i, join=None if i == 1 else 1, quota='4G')
    c.wait_members((1, 2, 3), 3)
    c.wait_voters((1, 2, 3), lambda n, v: k.voters_ok(n, v, 3))
    fails = []
    with ThreadPoolExecutor(PAR) as pool:
        for r in range(ROUNDS):
            rows = list(pool.map(lambda i: one(r, i), range(PAR)))
            bad = [x for x in rows if x[1] or x[2]]
            print(f'DQ round={r} copies={PAR} failed={len(bad)} busy_workers={sum(x[2] for x in rows)} '
                  f'max_s={max(x[3] for x in rows):.1f}', flush=True)
            fails += [f'round {r} copy {i}: rc={rc} busy_workers={b} {s:.1f} s\n{err[-1500:]}'
                      for i, rc, b, s, err in bad]
    assert not fails, '\n'.join(fails[:4])
    print(f'DQ STATUS slots apart from proof slots: {ROUNDS} rounds x {PAR} writers, '
          f'no BUSY stall: PASS ({time.time() - t0:.1f} s)')
finally:
    c.stop_all()
    if os.environ.get('DQ_KEEP') or sys.exc_info()[0] is not None:
        print('kept', work)
    else:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
