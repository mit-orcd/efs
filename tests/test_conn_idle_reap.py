#!/usr/bin/env python3
"""Pooled connections a burst opened are closed once idle (F1 follow-up,
efsd RSS on a fixed working set).

Background: efsd runs one thread per accepted connection, with its stack
and request buffers. The client pool (EFS_CLIENT_CONNS_PER_NODE slots per
server) and the server-to-server pool (34 per peer) grew to their busiest
moment and never shrank, so after a stall every connection it opened kept
its server thread for the life of the process: in the 60-minute
fixed-working-set D4 run the live connections rose from 46 to 65 on one
member with two retry storms and stayed. Each pool now closes a connection
idle for 30 s (its owner-less slots, scanned at most once a second on
checkout and release).

Three efsd and one efs-fuse on loopback (free ports, stores under TMPDIR).
BURST threads write and fsync their own 512 KiB files at once, three
rounds, which opens extra client and peer connections. Then one writer
keeps light traffic going (a small write and fsync every 0.5 s, so the
pools check out and release) for TRICKLE_S. Checks, on the sum of every
efsd's open sockets: the burst raised it by at least MIN_RISE (else the
test proves nothing), and after the trickle at most half of that rise is
left. Before the fix the count stays at the burst's peak.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, shutil, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, SLOW, Mount, fuse_ok  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('idle connection reap: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
BURST = 48
MIN_RISE = 12
IDLE_S = 30  # EFS_CLIENT_CONN_IDLE_MS, EFS_PEER_CONN_IDLE_MS
TRICKLE_S = IDLE_S + 15
work = Path(tempfile.mkdtemp(prefix='efs-reap-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
t0 = time.time()


def sockets(pid):
    n = 0
    for fd in os.listdir(f'/proc/{pid}/fd'):
        try:
            n += os.readlink(f'/proc/{pid}/fd/{fd}').startswith('socket:')
        except OSError:
            pass
    return n


c = Cluster(work, 'c', [1, 2, 3], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    m = Mount(c, 1, 'mnt')
    mounts.append(m)
    pids = [c.procs[i].pid for i in (1, 2, 3)]
    total = lambda: sum(sockets(p) for p in pids)  # noqa: E731
    (m.path / 'warm').write_bytes(b'w')
    time.sleep(1)
    before = total()
    peak = before
    errors = []
    data = os.urandom(512 * 1024)

    def burst(k, r):
        try:
            p = m.path / f'b{r}-{k}'
            with open(p, 'wb') as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
        except OSError as e:
            errors.append(f'{p}: {e}')

    for r in range(3):
        th = [threading.Thread(target=burst, args=(k, r)) for k in range(BURST)]
        for t in th:
            t.start()
        while any(t.is_alive() for t in th):
            peak = max(peak, total())
            time.sleep(0.05)
        peak = max(peak, total())
    assert not errors, errors[:4]
    rise = peak - before
    assert rise >= MIN_RISE, f'the burst opened only {rise} server sockets ({before} -> {peak})'
    end = time.time() + TRICKLE_S * SLOW
    n = 0
    while time.time() < end:
        with open(m.path / 'trickle', 'wb') as f:
            f.write(b't%d' % n)
            f.flush()
            os.fsync(f.fileno())
        n += 1
        time.sleep(0.5)
    after = total()
    print(f'idle connection reap: server sockets {before} -> peak {peak} -> {after} after {TRICKLE_S} s of light traffic')
    assert after - before <= rise // 2, f'idle connections kept: {after - before} of the burst\'s {rise} still open'
    for k in range(BURST):
        assert (m.path / f'b2-{k}').read_bytes() == data, f'b2-{k} differs'
    print(f'idle connection reap: PASS ({time.time() - t0:.1f} s)')
finally:
    for mm in mounts:
        mm.close()
    c.stop_all()
    if os.environ.get('EFS_REAP_KEEP'):
        print(f'kept {work}')
    else:
        shutil.rmtree(work, ignore_errors=True)
