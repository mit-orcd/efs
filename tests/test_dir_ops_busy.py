#!/usr/bin/env python3
"""Task O: twenty transient RMDIR admission BUSYs must not exhaust 16 tries.

Four private fault daemons; one normal FUSE client, TCP/O_DIRECT, 1 GiB/node.
One syscall, no application retry: twenty counted pre-admission refusals then
ordinary RMDIR. Check exact refusals, one immutable opid, and final absence.
Stores/logs stay in TMPDIR for the runner to archive and prune; logs are emitted.
"""
import os
from pathlib import Path
import re
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, Mount, fuse_ok, voters_ok  # noqa: E402

if not fuse_ok():
    print('Task O directory BUSY: NOTRUN (FUSE prerequisites absent)')
    raise SystemExit(0)
work = Path(tempfile.mkdtemp(prefix='efs-taskO-dir-', dir=os.environ.get('TMPDIR')))
marker = work / 'busy-count'
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_FAULT_RMDIR_BUSY_FILE=str(marker), EFS_TRANSPORT='tcp')
c = Cluster(work, 'rmdir', [1, 2, 3, 4], env)
m = None
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
    victim = m.path / 'task-o-busy'
    victim.mkdir()
    marker.write_text('20\n')
    start = time.monotonic()
    victim.rmdir()  # Exactly one syscall; no retry around application errors.
    elapsed = time.monotonic() - start
    assert not victim.exists()
    assert marker.read_text().strip() == '0'
    hits = re.findall(r'fault rmdir-admission BUSY remaining=(\d+) seq=(\d+)',
                      '\n'.join(c.log(i) for i in c.ports))
    assert len(hits) == 20, hits
    assert sorted(int(n) for n, _ in hits) == list(range(20)), hits
    assert len({seq for _, seq in hits}) == 1 and int(hits[0][1]) > 0, hits
    client = m.logp.read_text(errors='replace')
    assert re.search(r'inode-rpc: slow-ok .*attempts=21 .*saw_busy=1 .*status=0', client), client[-4000:]
    assert elapsed < 30, elapsed
    print(f'Task O directory BUSY: PASS 20 refusals, same opid, one RMDIR in {elapsed:.3f}s', flush=True)
finally:
    if m:
        m.close()
    c.stop_all()
    print(f'Task O fixture: {work}', flush=True)
    for log in c.dir.glob('*.log'):
        print(f'==== {log.name} ====\n'+log.read_text(errors='replace'), flush=True)
    # The runner owns TMPDIR retention/pruning; do not depend on a private
    # host cleanup binary or delete a failure before its logs are captured.
