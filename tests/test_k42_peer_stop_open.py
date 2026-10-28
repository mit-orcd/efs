#!/usr/bin/env python3
"""K42/W164: a clean unmount on a healthy cluster while another client holds
an OPEN append on a file this client appended to (Task A candidate D).

The stopping client's revoke must retire its append receipt on that inode;
the server refuses while the other session's OPEN reservation stands (its
REPORT resolves it). The fault hook EFS_FAULT_APPEND_HOLD_FILE (faults efsd)
holds that REPORT, so the OPEN stands as long as the test needs. Before K42 the refusal was a bare
BUSY and the unmount failed at once: `revoke rc=-13 phase=cleanup`, exit 1.
Now the server names the wait and the client repeats the step within its
20 s per-shard budget:

1. The primary's append stays OPEN for HOLD_S after the peer's unmount
   begins: the peer waits for it and exits 0 (revoke rc=0, >= HOLD_S).
2. A foreign OPEN past the budget (the hook's "cleanup" mode answers that
   wait for every append cleanup step; a real writer's REPORT bound and the
   server resolve an OPEN in about 16 s): the peer fails closed, exit 1, its
   log naming the shard and `reason=append-open-by-other-session`; once the
   wait ends, `efs-mgmt raft-session fence` completes the cleanup.

Three loopback daemons (tests/k33_daemons.py), two efs-fuse mounts. About
70 s; make test-long.
"""
import os, re, subprocess, sys, tempfile, threading, time
from datetime import datetime
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, Mount, fuse_ok, mgmt  # noqa: E402

HOLD_S = 3.0
BUDGET_S = 20.0


def stamp(line):
    return datetime.strptime(line[:23], '%Y-%m-%dT%H:%M:%S.%f').timestamp()


def append(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        os.write(fd, text.encode())
    finally:
        os.close(fd)


def unmount(m):
    """fusermount3 -u, then the client's exit code and its revoke line."""
    subprocess.run(['fusermount3', '-u', str(m.path)], check=True)
    return m


def finish(m, timeout=120):
    rc = m.p.wait(timeout=timeout)
    m.log.close()
    lines = m.logp.read_text(errors='replace').splitlines()
    rev = [l for l in lines if 'session revoked at unmount' in l]
    settled = [l for l in lines if 'put tickets outstanding at unmount' in l]
    assert rev and settled, '\n'.join(lines[-30:])
    return rc, rev[-1], stamp(rev[-1]) - stamp(settled[-1]), lines


def held(c, ino, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if any(f'EFS_FAULT_APPEND_HOLD_FILE holds ino={ino}' in c.log(i) for i in (1, 2, 3)):
            return
        time.sleep(0.05)
    raise AssertionError(f'append of ino {ino} never held')


def peer_round(c, primary, hold, name, hold_s, cleanup=False):
    """Peer appends to a fresh file; the primary's append to it stays OPEN
    for hold_s while the peer unmounts (cleanup: the hook answers the OPEN
    wait instead). Returns the peer's results."""
    peer = Mount(c, 2, name)
    writer = None
    try:
        f = f'{name}.log'
        append(peer.path / f, f'{name} first\n')
        append(peer.path / f, f'{name} second\n')
        ino = (primary.path / f).stat().st_ino
        hold.write_text('cleanup\n' if cleanup else f'{ino}\n')
        writer = threading.Thread(target=append, args=(primary.path / f, 'primary held\n'))
        writer.start()
        if not cleanup:
            held(c, ino)  # the primary's reservation is OPEN, its REPORT held
        t0 = time.monotonic()
        unmount(peer)
        remaining = hold_s - (time.monotonic() - t0)
        if remaining > 0:
            time.sleep(remaining)
        hold.unlink()
        writer.join(timeout=60)
        assert not writer.is_alive(), 'primary append still blocked after the hold'
        rc, rev, revoke_s, lines = finish(peer)
        data = (primary.path / f).read_text()
        assert data.count('\n') == 3 and 'primary held' in data, data
        return rc, rev, revoke_s, lines
    finally:
        # A failed unmount must stay a failure, but must not strand this
        # synthetic peer or a held REPORT after the enclosing cluster stops.
        if hold.exists():
            hold.unlink()
        if writer is not None:
            writer.join(timeout=60)
        if peer.p.poll() is None:
            peer.close()



def main():
    if not fuse_ok():
        print('NOTRUN: needs FUSE (fusermount3 and /dev/fuse)')
        return 0
    work = Path(tempfile.mkdtemp(prefix='efs-k42-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
    hold = work / 'append-hold'
    env['EFS_FAULT_APPEND_HOLD_FILE'] = str(hold)
    c = Cluster(work, 'c', [1, 2, 3], env)
    primary = None
    ok = False
    try:
        c.start(1)
        c.start(2, join=1)
        c.start(3, join=1)
        for i in (1, 2, 3):
            c.wait_log(i, r'raft-host: formed: 3 members', timeout=60)
        primary = Mount(c, 1, 'primary')

        # 1. A foreign OPEN shorter than the budget: the peer waits, exit 0.
        rc, rev, revoke_s, _ = peer_round(c, primary, hold, 'peer1', HOLD_S)
        assert rc == 0 and ' rc=0 ' in rev, rev
        assert revoke_s >= HOLD_S - 0.5, (revoke_s, rev)
        print(f'1. peer waited on the OPEN append: revoke {revoke_s:.1f} s, exit 0 PASS', flush=True)

        # 2. Past the budget: fail closed with shard and reason, resumable.
        rc, rev, revoke_s, lines = peer_round(c, primary, hold, 'peer2', BUDGET_S + 6, cleanup=True)
        assert rc == 1, (rc, rev)
        m = re.search(r'rc=-13 .*phase=cleanup shard=(\d+) reason=append-open-by-other-session', rev)
        assert m, rev
        assert revoke_s >= BUDGET_S - 1, (revoke_s, rev)
        assert any('unmount not clean' in l and 'revoke rc=-13' in l for l in lines), lines[-10:]
        uuid = [re.search(r'session uuid=([0-9a-f]{32})', l) for l in lines]
        uuid = [u.group(1) for u in uuid if u][-1]
        frc, out = mgmt(env, 'raft-session', c.ep(1), 'fence', uuid)
        assert frc == 0 and 'incomplete' not in out, out
        print(f'2. past the {BUDGET_S:.0f} s budget: exit 1, shard={m.group(1)} '
              f'reason=append-open-by-other-session; operator fence completes PASS', flush=True)
        ok = True
    finally:
        if primary:
            primary.close()
        c.stop_all()
        if ok:
            subprocess.run(['rm', '-rf', str(work)])
        else:
            print(f'kept {work}', flush=True)
    print('K42 peer stop with a foreign OPEN append: waits within budget, fails closed with reason PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
