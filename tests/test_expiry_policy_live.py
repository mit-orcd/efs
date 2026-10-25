#!/usr/bin/env python3
"""D43: the cluster expiry policy is the only expiry control.

Real ./efsd daemons (three, loopback, private stores under TMPDIR, free
ports, discovery off) and efs-fuse:
1. EFS_EXPIRY in the environment stops efsd at once (exit non-zero) with a
   message that names `efs-mgmt expiry-policy ... cluster set`.
2. Off by default, zero effect: `expiry-policy cluster get` answers version
   0, off; after several GC passes every node reports expiry_enabled=0 and
   expiry_passes=0 (the worker only read the policy); folder policies and
   deadline reads are refused; files created while off never get a record.
3. On at runtime (no restart): every node reports expiry_enabled=1 within a
   few GC passes; a file born under an enabled folder policy is removed
   after its deadline; files created while off still have no record.
4. Off at runtime: a due deadline does not act while off (the file stays,
   expiry_passes stops); on again, it is removed.
5. Coverage: a due file whose source folder policy is disabled stays; the
   folder policy enabled again, it is removed.
6. The cluster policy survives an orderly restart of every daemon.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons  # noqa: E402
from k33_daemons import Cluster, MGMT, Mount, fuse_ok, mgmt, SLOW  # noqa: E402

root = Path(__file__).resolve().parents[1]
EFSD = root / 'efsd'
k33_daemons.EFSD = EFSD  # the product daemon, not the fault build
for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('expiry policy live: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)

work = Path(tempfile.mkdtemp(prefix='efs-expiry-policy-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_FAULT', 'EFS_EXPIRY'))}
ACCES, STALE = -16, -14


def exact(s, n):
    out = b''
    while len(out) < n:
        part = s.recv(n - len(out))
        assert part, 'short reply'
        out += part
    return out


HELLO = struct.pack('>IB', 17, 165) + b'EFSP' + struct.pack('=IQ', 5, 0)


def rpc(c, tag, ino, gen=1, extra=b'', accepted=(0,)):
    """One metadata request (MSG 95) on the inode's group; follows
    NOT_PRIMARY/BUSY by trying every member. Returns (rc, index, salt, term)."""
    payload = bytes((0 if ino & 1 else 2, tag)) + struct.pack('>QQ', ino, gen) + extra
    end = time.time() + 20 * SLOW
    while True:
        for i in sorted(c.ports):
            with socket.create_connection(('127.0.0.1', c.ports[i]), timeout=15) as s:
                s.sendall(HELLO)
                n = struct.unpack('>I', exact(s, 4))[0]
                exact(s, n)
                s.sendall(struct.pack('>IB', len(payload) + 1, 95) + payload)
                n = struct.unpack('>I', exact(s, 4))[0]
                reply = exact(s, n)
            assert reply[0] == 96 and len(reply) == 33, reply
            rc, _, index, salt, term = struct.unpack('=iiQQQ', reply[1:])
            if rc in accepted:
                return rc, index, salt, term
            assert rc in (-13, -15), (tag, ino, rc)
        assert time.time() < end, ('request did not settle', tag, ino, rc)
        time.sleep(0.1)


def folder_policy(c, ino, ttl_s, flags, accepted=(0,)):
    """Operator (uid 0) folder policy through the trusted-client RPC, as
    tests/live/expiry_lifecycle.py does."""
    rc, rev, _, _ = rpc(c, 60, ino, accepted=(0, ACCES))
    if rc:
        return rc
    extra = struct.pack('>IQQQB', 0, rev, int(ttl_s * 1e9), int(max(ttl_s, 1) * 10 * 1e9), flags)
    return rpc(c, 58, ino, extra=extra, accepted=accepted)[0]


def record(c, path, accepted=(0,)):
    """(rc, revision, deadline_ns) of a file's expiry record."""
    rc, rev, deadline, _ = rpc(c, 61, path.stat().st_ino, accepted=accepted)
    return rc, rev, deadline


def cluster_policy(c, *args):
    rc, out = mgmt(env, 'expiry-policy', c.ep(1), 'cluster', *args)
    m = re.search(r'rc=(-?\d+) version=(\d+) expiry=(on|off|unknown)', out)
    assert rc == 0 and m and m.group(1) == '0', out
    return int(m.group(2)), m.group(3)


def gc(c, i):
    rc, out = mgmt(env, 'gc-status', c.ep(i))
    assert rc == 0, out
    got = {k: int(v) for k, v in re.findall(r'\b(expiry_enabled|expiry_passes)=(\d+)', out)}
    got['passes'] = int(re.search(r'^node=\d+ stage=\d+ group=\d+ exports=\d+ passes=(\d+)', out, re.M).group(1))
    return got


def wait(what, pred, timeout=30):
    end = time.time() + timeout * SLOW
    while time.time() < end:
        if pred():
            return
        time.sleep(0.25)
    raise AssertionError(what + ' timed out')


def create(path):
    with open(path, 'xb', buffering=0) as f:
        f.write(b'expiry policy ' * 64)
        os.fsync(f.fileno())
    return path


c = Cluster(work, 'x', [1, 2, 3], env)
m = None
ok = False
try:
    # 1. The removed environment switch refuses and names the policy.
    p = subprocess.run([str(EFSD), c.ep(1), str(work / 'refused')], capture_output=True, text=True,
                       env=dict(env, EFS_EXPIRY='1'), timeout=30)
    assert p.returncode != 0 and 'EFS_EXPIRY' in p.stderr and \
        'efs-mgmt expiry-policy NODE:PORT cluster set on|off' in p.stderr, p
    print('EFS_EXPIRY refused at startup, message names the cluster expiry policy PASS')

    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=60)
    m = Mount(c, 1, 'mnt')
    base = m.path / 'scratch'
    base.mkdir()
    assert cluster_policy(c, 'get') == (0, 'off')
    plain = create(base / 'born-off')
    wait('three GC passes on every node', lambda: all(gc(c, i).get('passes', 0) >= 3 for i in (1, 2, 3)))
    for i in (1, 2, 3):
        s = gc(c, i)
        assert s['expiry_enabled'] == 0 and s['expiry_passes'] == 0, (i, s)
    assert folder_policy(c, base.stat().st_ino, 4, 1, accepted=(ACCES,)) == ACCES
    assert record(c, plain, accepted=(ACCES,))[0] == ACCES
    print('off by default: version 0, no worker pass beyond the policy read, folder policy and records refused PASS')

    # 3. On at runtime.
    assert cluster_policy(c, 'set', 'on', '0') == (1, 'on')
    wait('every node sees the policy on', lambda: all(gc(c, i)['expiry_enabled'] == 1 for i in (1, 2, 3)))
    assert record(c, plain) == (0, 0, 0), 'a file born while off has a record'
    assert folder_policy(c, base.stat().st_ino, 4, 1) == 0
    born = create(base / 'born-on')
    _, rev, deadline = record(c, born)
    assert rev == 1 and deadline > time.time_ns(), (rev, deadline)
    wait('deadline removes the file', lambda: not born.exists(), 40)
    assert plain.exists()
    print('on at runtime: every node within GC passes, deadline acts, files born while off untouched PASS')

    # 4. Off at runtime: a due deadline waits.
    held = create(base / 'held-by-off')
    assert record(c, held)[1] == 1
    assert cluster_policy(c, 'set', 'off', '1') == (2, 'off')
    wait('every node sees the policy off', lambda: all(gc(c, i)['expiry_enabled'] == 0 for i in (1, 2, 3)))
    passes = [gc(c, i)['expiry_passes'] for i in (1, 2, 3)]
    time.sleep(7 * SLOW)  # deadline (4 s) passed with margin
    assert held.exists(), 'a deadline acted while the cluster policy was off'
    assert [gc(c, i)['expiry_passes'] for i in (1, 2, 3)] == passes, 'expiry worker ran while off'
    assert cluster_policy(c, 'set', 'on', '2') == (3, 'on')
    wait('due file removed once on again', lambda: not held.exists(), 40)
    print('off at runtime: due deadline and worker paused; on again, it acts PASS')

    # 5. Coverage: a disabled source folder policy stops its deadlines.
    covered = create(base / 'uncovered')
    assert record(c, covered)[1] == 1
    assert folder_policy(c, base.stat().st_ino, 4, 0) == 0
    time.sleep(7 * SLOW)
    assert covered.exists(), 'a deadline acted after its folder policy was disabled'
    assert record(c, covered)[2] > 0  # the deadline is kept, not acted on
    assert folder_policy(c, base.stat().st_ino, 4, 1) == 0
    wait('covered again: removed', lambda: not covered.exists(), 40)
    print('coverage: disabled folder policy keeps the file; enabled again, removed PASS')

    # 6. Persistence across an orderly restart of every daemon.
    m.close()
    m = None
    c.stop_all()
    c.held = []
    for i in (1, 2, 3):
        c.restart(i)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: up raft_id=\d+ n=3 formed', timeout=60)
    wait('policy readable after restart', lambda: mgmt(env, 'expiry-policy', c.ep(1), 'cluster', 'get')[0] == 0, 60)
    assert cluster_policy(c, 'get') == (3, 'on')
    wait('every node on after restart', lambda: all(gc(c, i)['expiry_enabled'] == 1 for i in (1, 2, 3)), 60)
    print('cluster expiry policy survives an orderly restart PASS')
    ok = True
finally:
    if m:
        m.close()
    c.stop_all()
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('kept for diagnosis:', work)
print('expiry policy live: D43 policy-only control, zero effect off, runtime on/off, coverage, restart PASS')
