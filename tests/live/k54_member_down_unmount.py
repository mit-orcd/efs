#!/usr/bin/env python3
"""K54: clean unmount with one storage member down (D46 item 6, finding U4).

Usage: python3 tests/live/k54_member_down_unmount.py [--runs N] [--members 4]
           [--mode alternate|kill|stop] [--down 1] [--victim 3] [--port-base 18700] [--keep]

Run from the root of a built tree (make, make tests/faults/efsd). Starts
--members private efsd daemons on 127.0.0.1 (PORT_BASE+1..) and, per run:
mounts a client on node 1, writes and fsyncs a few files, stops --down
members (from --victim up; not node 1) by SIGKILL or SIGTERM (alternate: odd runs SIGKILL,
even runs SIGTERM), unmounts the client with `scripts/client.sh stop`, and
requires exit 0 and `session revoked at unmount ... rc=0`. Then it restarts
the stopped members, waits until `efs-mgmt status` shows every member up and
no resync pending, mounts a fresh client and reads every acknowledged file
back byte for byte. With --down 2 on four members the unmount must FAIL
closed (exit != 0, `rc=-13 phase=storage-fence`): that is the negative
control, and it counts as a pass. One line per run, then `PASS n/n`.
"""
import argparse, os, re, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from k33_daemons import Cluster, Mount, mgmt, payload  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def revoke_line(log):
    rev = [l for l in log.splitlines() if 'session revoked at unmount' in l]
    return rev[-1] if rev else ''


def wait_in_sync(c, ids, timeout=120):
    end, out = time.time() + timeout, ''
    while time.time() < end:
        rc, out = mgmt(c.env, 'status', c.ep(1))
        if (rc == 0 and re.search(r'Cluster state: OK \(%d up, 0 down\)' % len(ids), out) and
                'Protection: full' in out and re.search(r'Degraded segments: 0 ', out)):
            return out
        time.sleep(1)
    raise AssertionError('cluster never in sync:\n' + out)


def one_run(c, ids, run, mode, down, files, first=3):
    name = f'm{run:03d}'
    victims = ids[first - 1:first - 1 + down]
    m = Mount(c, 1, name)
    names = []
    for k in range(files):
        f = f'{name}-f{k}'
        data = payload(f, 65536 + k * 4099)
        fd = os.open(m.path / f, os.O_WRONLY | os.O_CREAT, 0o644)
        os.write(fd, data)
        os.fsync(fd)
        os.close(fd)
        names.append((f, data))
    sig = signal.SIGKILL if (mode == 'kill' or (mode == 'alternate' and run % 2)) else signal.SIGTERM
    for v in victims:
        c.stop(v, sig)
    t0 = time.time()
    st = subprocess.run([str(ROOT / 'scripts/client.sh'), 'stop', str(m.path)],
                        capture_output=True, text=True, timeout=300)
    try:
        rc = m.p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        rc = None
    m.log.close()
    line = revoke_line(m.logp.read_text(errors='replace'))
    secs = time.time() - t0
    print(f'  stop_rc={st.returncode} exit={rc} {line}', flush=True)
    clean = st.returncode == 0 and rc == 0 and re.search(r'\brc=0\b', line) is not None
    for v in victims:
        c.restart(v)
        c.wait_log(v, r'listening on', timeout=60)
    wait_in_sync(c, ids)
    m2 = Mount(c, 1, name + 'r')
    bad = [f for f, data in names if (m2.path / f).read_bytes() != data]
    m2.close()
    return sig.name, clean, rc, line, secs, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=20)
    ap.add_argument('--members', type=int, default=4)
    ap.add_argument('--down', type=int, default=1)
    ap.add_argument('--victim', type=int, default=3, help='first member stopped (2..members)')
    ap.add_argument('--files', type=int, default=4)
    ap.add_argument('--mode', choices=('alternate', 'kill', 'stop'), default='alternate')
    ap.add_argument('--port-base', type=int, default=18700)
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    work = Path(tempfile.mkdtemp(prefix='efs-k54-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
    ids = list(range(1, a.members + 1))
    c = Cluster(work, 'c', ids, env, disc_port=a.port_base + 99)
    for h in c.held:
        h.close()
    c.held = []
    c.ports = {i: a.port_base + i for i in ids}
    expect_fail = a.down > a.members - (a.members // 2 + 1)
    failed = done = 0
    try:
        c.start(1)
        for i in ids[1:]:
            c.start(i, join=1)
        for i in ids:
            c.wait_log(i, r'(formed: %d members|n=%d formed|membership revision \d+: %d members)'
                       % (a.members, a.members, a.members), timeout=120, run=False)
        for run in range(1, a.runs + 1):
            sig, clean, rc, line, secs, bad = one_run(c, ids, run, a.mode, a.down, a.files, a.victim)
            if expect_fail:
                ok = not clean and 'phase=storage-fence' in line and not bad
            else:
                ok = clean and not bad
            done += 1
            failed += not ok
            print(f'run={run} signal={sig} down={a.down} exit={rc} unmount_s={secs:.1f} '
                  f'readback_bad={len(bad)} ok={int(ok)}\n  {line}', flush=True)
    finally:
        c.stop_all()
        print(f'{"PASS" if done and not failed else "FAIL"} {done - failed}/{done}', flush=True)
        if a.keep or failed:
            print(f'kept {work}', flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if failed or not done else 0


if __name__ == '__main__':
    sys.exit(main())
