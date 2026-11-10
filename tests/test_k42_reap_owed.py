#!/usr/bin/env python3
"""K42 (reopened Nov 10): a clean unmount must not wait for the GC reaper.

A client appends to files and unlinks them; each dead inode owes REAP_MULTI
its reservation and receipt rows, and the GC thread of the group leader is
the only pass that performs it (a round-robin queue shared by every dead
inode of the cluster). The unmount's revoke used to poll for that reap and,
on a busy cluster, spent the whole 20 s cleanup budget refused with
`reason=append-reap-owed` (Task EY, 4-member RDMA cell). The revoke now
removes its own session's rows of a dead inode itself, so the clean stop does
not depend on the reaper.

The test stops the reaper (EFS_FAULT_GC_DISABLE, tests/faults/efsd), so the
queue never advances: before the fix the unmount fails (exit 1, `rc=-13`
and the reason); after it every round exits 0 with `rc=0`. `--gc-on` leaves
the reaper running (the natural race, needs many files to lose it).

Usage: python3 tests/test_k42_reap_owed.py [--members 3] [--rounds 1] [--files 24]
           [--port-base N] [--gc-on] [--live] [--keep]

One line per round:
  round=K exit=RC revoke_rc=RC revoke_ms=MS cleanup_ms=MS touched=T cleaned=C reason=R
then `PASS N/N` or `FAIL k/N`. Needs FUSE and `make tests/faults/efsd`.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, Mount, fuse_ok  # noqa: E402


def churn(path, files, unlink=True):
    """Create, O_APPEND twice, close and unlink: the dead inodes keep this
    session's reservation and receipt rows until the reaper removes them."""
    for k in range(files):
        p = path / f'dead-{k:05d}'
        for text in (f'first {k}\n', f'second {k}\n'):
            fd = os.open(p, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
            try:
                os.write(fd, text.encode())
            finally:
                os.close(fd)
    for k in range(files if unlink else 0):
        os.unlink(path / f'dead-{k:05d}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--members', type=int, default=3)
    ap.add_argument('--rounds', type=int, default=1)
    ap.add_argument('--files', type=int, default=24)
    ap.add_argument('--port-base', type=int, default=0)
    ap.add_argument('--gc-on', action='store_true')
    ap.add_argument('--live', action='store_true', help='leave the files linked (control)')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    if not fuse_ok():
        print('NOTRUN: needs FUSE (fusermount3 and /dev/fuse)')
        return 0
    work = Path(tempfile.mkdtemp(prefix='efs-k42-reap-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
    if not a.gc_on:
        env['EFS_FAULT_GC_DISABLE'] = '1'
    ids = list(range(1, a.members + 1))
    c = Cluster(work, 'c', ids, env, disc_port=a.port_base + 99 if a.port_base else 0)
    if a.port_base:
        for h in c.held:
            h.close()
        c.held = []
        c.ports = {i: a.port_base + i for i in ids}
    failed = done = 0
    try:
        c.start(1)
        for i in ids[1:]:
            c.start(i, join=1)
        for i in ids:
            c.wait_log(i, r'(formed: %d members|n=%d formed|membership revision \d+: %d members)'
                       % (a.members, a.members, a.members), timeout=120, run=False)
        for run in range(1, a.rounds + 1):
            node = ids[run % len(ids)]
            m = Mount(c, node, f'm{run:03d}')
            churn(m.path, a.files, not a.live)
            subprocess.run(['fusermount3', '-u', str(m.path)], check=True)
            try:
                rc = m.p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                rc = None
            m.log.close()
            rev = [l for l in m.logp.read_text(errors='replace').splitlines()
                   if 'session revoked at unmount' in l]
            f = dict(re.findall(r'(\w+)=(\S+)', rev[-1])) if rev else {}
            ok = rc == 0 and f.get('rc') == '0'
            done += 1
            failed += not ok
            print(f'round={run} node={node} exit={rc} revoke_rc={f.get("rc", "?")} '
                  f'revoke_ms={f.get("ms", "?")} cleanup_ms={f.get("cleanup_ms", "?")} '
                  f'touched={f.get("touched", "?")} cleaned={f.get("cleaned", "?")} '
                  f'reason={f.get("reason", "-")}', flush=True)
            if not ok:
                print(f'  {rev[-1] if rev else m.logp.read_text(errors="replace")[-800:]}', flush=True)
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
