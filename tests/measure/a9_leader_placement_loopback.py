#!/usr/bin/env python3
"""A9: leader placement on a private loopback cluster (scripts/leader_placement.py).

Starts N product efsd daemons (./efsd, TCP) on free loopback ports with
private stores under --work (default TMPDIR), forms the cluster by itself
(seed, then joiners through it), runs scripts/leader_placement.py `run`
against the seed with the given step-downs and restarts, and stops and
deletes everything it started. A restart stops the leader's daemon
(SIGTERM), keeps it down --down seconds and starts it again from its store
(tests/k33_daemons.py Cluster.restart). Nothing outside the daemons this
script starts is touched.

  python3 tests/measure/a9_leader_placement_loopback.py --members 3 \\
      --duration 600 --stepdowns 12 --restarts 6 --json OUT.json

Prints the tables of scripts/leader_placement.py; with --json the samples
and events are written for `leader_placement.py report OUT.json`.
"""
import argparse, os, shutil, sys, tempfile, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tests'))
sys.path.insert(0, str(root / 'scripts'))
import k33_daemons  # noqa: E402
import leader_placement  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--members', type=int, required=True)
ap.add_argument('--duration', type=float, default=600.0)
ap.add_argument('--interval', type=float, default=0.5)
ap.add_argument('--stepdowns', type=int, default=12)
ap.add_argument('--restarts', type=int, default=6)
ap.add_argument('--down', type=float, default=5.0)
ap.add_argument('--json')
ap.add_argument('--work', default=os.environ.get('TMPDIR'))
a = ap.parse_args()
if a.members < 3:
    sys.exit('--members must be at least 3 (a cluster forms at three)')

k33_daemons.EFSD = root / 'efsd'   # the product daemon, not the EFS_FAULTS build
for b in (k33_daemons.EFSD, k33_daemons.MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
env = dict(os.environ, EFS_TRANSPORT='tcp')
work = Path(tempfile.mkdtemp(prefix='efs-a9-placement-', dir=a.work))
c = k33_daemons.Cluster(work, f'm{a.members}', list(range(1, a.members + 1)), env)
try:
    c.start(1)
    for i in range(2, a.members + 1):
        c.start(i, join=1)
    ids = list(range(1, a.members + 1))
    c.wait_members(ids, a.members, timeout=120)
    led = c.leader_placement(1, timeout=60)
    print(f'formed: {a.members} members, leaders {led}', flush=True)
    by_id = {c.node_id(i): i for i in ids}

    def restart(action, nid, ep):
        i = by_id[nid]
        if action == 'stop':
            c.stop(i)
            return 0
        c.restart(i)
        return 0

    args = argparse.Namespace(seed=c.ep(1), duration=a.duration, interval=a.interval,
                              stepdowns=a.stepdowns, restarts=a.restarts, down=a.down,
                              restart_cmd=None, mgmt=str(k33_daemons.MGMT), json=a.json,
                              label=f'loopback {a.members} members, TCP')
    leader_placement.run(args, restart=restart)
finally:
    c.stop_all()
    shutil.rmtree(work, ignore_errors=True)
