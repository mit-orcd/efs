#!/usr/bin/env python3
"""D43 measurement: file-create cost with the cluster expiry policy off and on.

Three ./efsd daemons on loopback (tests/k33_daemons.py, private stores under
TMPDIR, free ports) and one efs-fuse mount. Each sample creates FILES new
files (O_CREAT|O_EXCL, 4 KiB write, fsync, close) in a fresh directory and
records the wall time of each create+write+fsync+close. Modes, measured in
turn ROUNDS times (off, on, on+folder, off, ...):
  off         cluster expiry policy off (the default)
  on          policy on, no folder policy enabled anywhere
  on+folder   policy on, an enabled folder policy (1 h) on the directory
  on+elsewhere  policy on, an enabled folder policy on a sibling directory
              only (none above the measured one)
Each folder policy is disabled after its sample, so every "on" sample runs
with no enabled folder policy in the cluster.
Prints one JSON object: per mode count, mean, p50, p90, p99 and max in ms.
Not part of make test: a measurement for the D43 evidence page.

  python3 tests/measure/expiry_create_cost.py [--files 300] [--rounds 3]
"""
import argparse, json, os, shutil, socket, statistics, struct, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import k33_daemons  # noqa: E402
from k33_daemons import Cluster, Mount, fuse_ok, mgmt  # noqa: E402

root = Path(__file__).resolve().parents[2]
k33_daemons.EFSD = root / 'efsd'
HELLO = struct.pack('>IB', 17, 165) + b'EFSP' + struct.pack('=IQ', 5, 0)


def exact(s, n):
    out = b''
    while len(out) < n:
        part = s.recv(n - len(out))
        assert part, 'short reply'
        out += part
    return out


def rpc(c, tag, ino, extra=b''):
    payload = bytes((0 if ino & 1 else 2, tag)) + struct.pack('>QQ', ino, 1) + extra
    for _ in range(100):
        for port in c.ports.values():
            with socket.create_connection(('127.0.0.1', port), timeout=15) as s:
                s.sendall(HELLO)
                exact(s, struct.unpack('>I', exact(s, 4))[0])
                s.sendall(struct.pack('>IB', len(payload) + 1, 95) + payload)
                reply = exact(s, struct.unpack('>I', exact(s, 4))[0])
            rc, _, index, _, _ = struct.unpack('=iiQQQ', reply[1:])
            if rc == 0:
                return index
            assert rc in (-13, -15), (tag, rc)
        time.sleep(0.1)
    raise AssertionError('request did not settle')


def set_cluster(c, env, on):
    out = mgmt(env, 'expiry-policy', c.ep(1), 'cluster', 'get')[1]
    version = int(out.split('version=')[1].split()[0])
    rc, out = mgmt(env, 'expiry-policy', c.ep(1), 'cluster', 'set', 'on' if on else 'off', str(version))
    assert rc == 0, out
    end = time.time() + 30
    while time.time() < end:
        if all(f'expiry_enabled={int(on)}' in mgmt(env, 'gc-status', c.ep(i))[1] for i in c.ports):
            return
        time.sleep(0.25)
    raise AssertionError('policy change not seen')


def sample(directory, files):
    out = []
    body = b'x' * 4096
    for i in range(files):
        t = time.perf_counter()
        fd = os.open(directory / f'f{i}', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.write(fd, body)
        os.fsync(fd)
        os.close(fd)
        out.append((time.perf_counter() - t) * 1000)
    return out


def summary(v):
    v = sorted(v)
    q = lambda f: v[min(len(v) - 1, int(f * len(v)))]
    return dict(count=len(v), mean=round(statistics.fmean(v), 2), p50=round(q(.5), 2),
                p90=round(q(.9), 2), p99=round(q(.99), 2), max=round(v[-1], 2))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--files', type=int, default=300)
    p.add_argument('--rounds', type=int, default=3)
    a = p.parse_args()
    if not fuse_ok():
        print('expiry create cost: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
        return 0
    work = Path(tempfile.mkdtemp(prefix='efs-expiry-cost-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_FAULT', 'EFS_EXPIRY'))}
    c = Cluster(work, 'x', [1, 2, 3], env)
    m = None
    results = {'off': [], 'on': [], 'on+folder': [], 'on+elsewhere': []}
    try:
        c.start(1)
        c.start(2, join=1)
        c.start(3, join=1)
        for i in (1, 2, 3):
            c.wait_log(i, r'raft-host: formed: 3 members', timeout=60)
        m = Mount(c, 1, 'mnt')
        sample(m.path, 20)  # warm-up: sessions, connections, lanes
        for r in range(a.rounds):
            for mode in results:
                set_cluster(c, env, mode != 'off')
                d = m.path / f'{mode}-{r}'
                d.mkdir()
                policy = None
                if mode in ('on+folder', 'on+elsewhere'):
                    policy = d if mode == 'on+folder' else m.path / f'{mode}-{r}-sibling'
                    policy.mkdir(exist_ok=True)
                    ino = policy.stat().st_ino
                    rev = rpc(c, 60, ino)
                    rpc(c, 58, ino, struct.pack('>IQQQB', 0, rev, 3600 * 10**9, 7200 * 10**9, 1))
                results[mode] += sample(d, a.files)
                if policy:
                    rpc(c, 58, ino, struct.pack('>IQQQB', 0, rev + 1, 3600 * 10**9, 7200 * 10**9, 0))
        print(json.dumps({'files_per_sample': a.files, 'rounds': a.rounds, 'host': socket.gethostname(),
                          'ms': {k: summary(v) for k, v in results.items()}}, indent=1))
    finally:
        if m:
            m.close()
        c.stop_all()
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
