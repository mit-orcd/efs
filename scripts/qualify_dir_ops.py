#!/usr/bin/env python3
"""Task O retained four-member POSIX core stress. Private loopback only.

Build first; run from source with --work and --output outside the source tree.
Optional --load-tree arguments name separately built copies for two concurrent
unchanged test-long pools. No application outcomes are retried into PASS.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from k33_daemons import Cluster, Mount, fuse_ok, voters_ok  # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--rounds', type=int, default=10)
    p.add_argument('--load-tree', type=Path, action='append', default=[])
    a = p.parse_args()
    a.work.mkdir(parents=True, exist_ok=False)
    a.output.mkdir(parents=True, exist_ok=False)
    if not fuse_ok():
        raise SystemExit('Task O: NOTRUN (FUSE prerequisites absent)')
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
    env['EFS_RAFT_OBS'] = '1'
    c = Cluster(a.work, 'cluster', [1, 2, 3, 4], env)
    m = None
    loads = []
    records = []
    try:
        c.start(1)
        c.start(2, join=1)
        c.start(3, join=1)
        for i in (1, 2, 3):
            c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
        c.start(4, join=1)
        c.wait_members((1, 2, 3, 4), 4, timeout=120)
        c.wait_voters((1, 2, 3, 4), lambda i, v: voters_ok(i, v, 4), timeout=120)
        m = Mount(c, 2, 'mnt')
        for n, tree in enumerate(a.load_tree):
            f = (a.output / f'load-{n}.log').open('x')
            e = dict(env, EFS_TEST_TARGET='test-long', EFS_TEST_JOBS='8')
            proc = subprocess.Popen(['bash', 'scripts/make_test_summary.sh'], cwd=tree,
                                    env=e, stdout=f, stderr=subprocess.STDOUT)
            loads.append((proc, f))
        for n in range(a.rounds):
            start = time.monotonic()
            log = a.output / f'core-{n}.log'
            with log.open('x') as f:
                rc = subprocess.run([sys.executable, str(ROOT / 'tests/posix/posix_suite.py'),
                                     str(m.path), '--suite', 'core', '--results',
                                     str(a.output / f'core-{n}.json')], env=env,
                                    stdout=f, stderr=subprocess.STDOUT, timeout=900).returncode
            records.append(dict(round=n, rc=rc, seconds=time.monotonic()-start))
            (a.output / 'rounds.json').write_text(json.dumps(records, indent=2)+'\n')
            print(records[-1], flush=True)
            if rc:
                break
        for proc, f in loads:
            proc.wait(timeout=1800)
            f.close()
        return int(any(r['rc'] for r in records))
    finally:
        if m:
            m.close()
        c.stop_all()
        for proc, f in loads:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=30)
            f.close()
        # Retain failed stores while investigated and all original logs.
        import shutil
        for src in c.dir.glob('*.log'):
            shutil.copy2(src, a.output / src.name)


if __name__ == '__main__':
    raise SystemExit(main())
