#!/usr/bin/env python3
"""Diagnose K54 on a fresh untouched-product mount, never accept a dirty exit.

The expected failure is evidence, not a passing AN matrix cell. All fixtures
and client/daemon logs remain on the originating host.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import signal
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
assert socket.gethostname().split('.')[0] == 'fcstor014'
assert not a.work.exists() and not a.out.exists()
sys.path.insert(0, str(a.source / 'scripts'))
from task_i_host_sample import foreign_jobs
for i in range(2):
    jobs = foreign_jobs(); assert not jobs, jobs
    if not i: time.sleep(20)
sys.path.insert(0, str(a.source / 'tests'))
import k33_daemons as k
a.work.mkdir(parents=True); a.out.mkdir(parents=True)
env = {n:v for n,v in os.environ.items() if not n.startswith('EFS_')}
env.update(EFS_TRANSPORT='rdma', EFS_RDMA_DEV='mlx5_2')
c = k.Cluster(a.work, 'cluster', [1,2,3], env)
m = None
result = dict(verdict='FAIL', purpose='untouched-base K54 diagnostic', fixture=str(a.work))
try:
    for i in (1,2,3): c.start(i, join=None if i == 1 else 1, quota='8G')
    c.wait_members((1,2,3), 3)
    k.place_leaders(c, 1, groups=(0,))
    k.place_leaders(c, 2, groups=(2,))
    m = k.Mount(c, 2, 'readonly')
    os.stat(m.path)
    c.stop(3, signal.SIGKILL)
    end = time.monotonic() + 30
    while time.monotonic() < end:
        tabs = [c.members(i) for i in (1,2)]
        if all(3 in t and t[3][1] == 'down' for t in tabs): break
        time.sleep(.2)
    else: raise AssertionError('member not DOWN within 30 s')
    m.close()
    log = m.logp.read_text()
    result.update(fuse_exit=m.p.returncode,
                  storage_fence_failure='phase=storage-fence' in log)
    assert m.p.returncode == 1 and result['storage_fence_failure'], log[-2000:]
    result['diagnostic'] = 'K54 reproduced; this remains a failed clean unmount'
finally:
    if m and m.p.poll() is None: m.close()
    c.stop_all()
    (a.out / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)
