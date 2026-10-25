#!/usr/bin/env python3
"""E2 clean stop of a loaded peer client on a healthy cluster (K42/W164).

Usage: python3 tests/live/e2_peer_stop_loop.py [--runs N] [--until-fail]
           [--members 5] [--port-base 18500] [--suite] [--keep]

Run from the root of a built tree (make, make tests/faults/efsd). Starts
--members private efsd daemons on 127.0.0.1 ports PORT_BASE+1.. (discovery
PORT_BASE+99) and a primary efs-fuse on node 1. Each run mounts a peer
efs-fuse on node 2..N in turn, loads both clients (both append O_APPEND
records to the same shared files and write private files, and with --suite
also run tests/posix/posix_2client.py), then stops the peer with
`scripts/client.sh stop` while the primary keeps appending. A run passes when
the stop exits 0, the peer efs-fuse exits 0 and its log says
`session revoked at unmount: ... rc=0`. One line per run:
  run=K node=I stop=RC exit=RC touched=T revoke_ms=MS cleaned=C ok=0|1
then `PASS N/N` or `FAIL k/N`. The cluster work directory is deleted at the
end unless --keep or a run failed (then the path is printed, with daemon
and client logs, for the evidence summary). Needs fusermount3.
"""
import argparse, errno, os, re, shutil, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from k33_daemons import Cluster, Mount, payload  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def appender(path, tag, stop, errors):
    """O_APPEND records into the shared files until stop is set."""
    k = 0
    while not stop.is_set():
        try:
            for f in range(4):
                fd = os.open(path / f'shared-{f}', os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
                try:
                    os.write(fd, f'{tag} {k:08d}\n'.encode())
                finally:
                    os.close(fd)
            if k % 16 == 0:
                p = path / f'{tag}-private-{k % 64}'
                p.write_bytes(payload(f'{tag}-{k}', 4096 + k % 3 * 65536))
                if k % 32 == 0:
                    try:
                        p.unlink()
                    except OSError as e:
                        # A file still being written back may refuse; not this test's subject.
                        if e.errno != errno.EBUSY:
                            raise
        except OSError as e:
            errors.append(f'{tag}: {e}')
            return
        k += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=20)
    ap.add_argument('--until-fail', action='store_true')
    ap.add_argument('--members', type=int, default=5)
    ap.add_argument('--port-base', type=int, default=18500)
    ap.add_argument('--load-s', type=float, default=8.0)
    ap.add_argument('--suite', action='store_true', help='also run posix_2client.py each run')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    work = Path(tempfile.mkdtemp(prefix='efs-e2-peer-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
    ids = list(range(1, a.members + 1))
    c = Cluster(work, 'c', ids, env, disc_port=a.port_base + 99)
    for h in c.held:
        h.close()
    c.held = []
    c.ports = {i: a.port_base + i for i in ids}
    failed = 0
    done = 0
    primary = None
    try:
        c.start(1)
        for i in ids[1:]:
            c.start(i, join=1)
        for i in ids:
            c.wait_log(i, r'(formed: %d members|n=%d formed|membership revision \d+: %d members)'
                       % (a.members, a.members, a.members), timeout=120, run=False)
        primary = Mount(c, 1, 'primary')
        shared = primary.path / 'shared'
        shared.mkdir()
        run = 0
        while a.until_fail or run < a.runs:
            run += 1
            node = ids[1 + (run - 1) % (len(ids) - 1)] if len(ids) > 1 else 1
            name = f'peer{run:03d}'
            peer = Mount(c, node, name)
            subprocess.run(['timeout', '30', 'stat', str(peer.path / 'shared')], check=True,
                           capture_output=True)
            errors = []
            keep, peer_stop = threading.Event(), threading.Event()
            prim = threading.Thread(target=appender, args=(shared, 'p', keep, errors))
            load = threading.Thread(target=appender, args=(peer.path / 'shared', name, peer_stop, errors))
            prim.start()
            load.start()
            if a.suite:
                parent = f'suite-{run:03d}'
                r = subprocess.run(['python3', str(ROOT / 'tests/posix/posix_2client.py'), '--local',
                                    str(primary.path), str(peer.path), '--parent', parent,
                                    '--results', str(c.dir / f'{name}-posix2.tsv')],
                                   capture_output=True, text=True, timeout=900)
                (c.dir / f'{name}-posix2.out').write_text(r.stdout + r.stderr)
            time.sleep(a.load_s)
            # The peer's own load ends; the primary keeps appending through the stop.
            peer_stop.set()
            load.join()
            st = subprocess.run([str(ROOT / 'scripts/client.sh'), 'stop', str(peer.path)],
                                capture_output=True, text=True, timeout=900)
            (c.dir / f'{name}-stop.out').write_text(st.stdout + st.stderr)
            try:
                rc = peer.p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                rc = None
            keep.set()
            prim.join()
            peer.log.close()
            log = peer.logp.read_text(errors='replace')
            rev = [l for l in log.splitlines() if 'session revoked at unmount' in l]
            m = re.search(r'touched=(\d+) rc=(-?\d+) ms=(\d+).*cleaned=(\d+)', rev[-1]) if rev else None
            ok = (st.returncode == 0 and rc == 0 and m is not None and m.group(2) == '0'
                  and not errors)
            done += 1
            failed += not ok
            print(f'run={run} node={node} stop={st.returncode} exit={rc} '
                  f'touched={m.group(1) if m else "?"} revoke_rc={m.group(2) if m else "?"} '
                  f'revoke_ms={m.group(3) if m else "?"} cleaned={m.group(4) if m else "?"} '
                  f'ok={int(ok)}' + (f' errors={errors[:2]}' if errors else ''), flush=True)
            if not ok:
                print(f'  {rev[-1] if rev else log[-800:]}', flush=True)
                if a.until_fail:
                    break
            elif not a.keep:
                for p in c.dir.glob(f'{name}*'):
                    if p.is_file():
                        p.unlink()
                try:
                    peer.path.rmdir()
                except OSError:
                    pass
    finally:
        if primary:
            primary.close()
        c.stop_all()
        print(f'{"PASS" if done and not failed else "FAIL"} {done - failed}/{done}', flush=True)
        if a.keep or failed:
            print(f'kept {work}', flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if failed or not done else 0


if __name__ == '__main__':
    sys.exit(main())
