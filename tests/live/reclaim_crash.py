#!/usr/bin/env python3
"""W124 crash gate: SIGKILL storage daemons while batched reclaim is busy.

Private 3-node cluster (Linux/FUSE) on fresh roots, run from a built tree:

  python3 tests/live/reclaim_crash.py --base /tmp/efs-rc --port 17731 \
      --files 4000 --big 8 --rounds 12

1. create FILES keep and FILES doom tiny files plus BIG 4 MiB doom files,
   sync, record every inode;
2. unlink the doom set (from the mount), then for ROUNDS rounds SIGKILL a
   random daemon after a random 0.1..2 s and restart it (journal replay
   runs before it listens again);
3. wait until no doom inode has a fragment on any root and every node's
   reclaim journal is empty and not fail-closed;
4. read every keep file back and compare bytes;
5. cleanly restart all daemons and require each node's data_usage_bytes
   (a fresh startup inventory) to equal its pre-restart value: the quota
   credit and settle were exact through the crashes.

A SIGKILLed root refuses COMMIT until reformat (W71), so no files are
written after the first kill. Exit 0 = PASS. Logs and roots stay in BASE
for diagnosis; delete BASE afterwards.
"""
import argparse, hashlib, os, random, re, signal, socket, subprocess, sys, time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--base', type=Path, required=True)
p.add_argument('--port', type=int, default=17731)
p.add_argument('--files', type=int, default=4000)
p.add_argument('--big', type=int, default=8)
p.add_argument('--rounds', type=int, default=12)
p.add_argument('--seed', type=int, default=124)
p.add_argument('--drain-seconds', type=int, default=900)
a = p.parse_args()
src = Path(__file__).resolve().parents[2]
rng = random.Random(a.seed)
base = a.base
assert not base.exists() or not any(base.iterdir()), 'base must be empty'
roots = [base / f'n{i+1}' for i in range(3)]
mount = base / 'mnt'
for d in roots + [mount]:
    d.mkdir(parents=True, exist_ok=True)
# W125: keep the packer out of the way (an hour's minimum age) so every keep
# file stays in per-file fragments; this gate checks W124 whole-inode reclaim.
env = dict(os.environ, EFS_TRANSPORT='tcp', EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0', EFS_MD_RAFT_N='3',
           EFS_PACK_MIN_AGE_MS='3600000')
procs = [None, None, None]
logs = []
client = None


def log(msg):
    print(time.strftime('%H:%M:%S ') + msg, flush=True)


def listening(port):
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=.2):
            return True
    except OSError:
        return False


def wait(pred, what, seconds=60):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if pred():
            return
        time.sleep(.2)
    raise AssertionError(what + ' timed out')


def start(i):
    f = open(base / f'n{i+1}.log', 'ab'); logs.append(f)
    cmd = [str(src / 'efsd'), '--node-id', str(i + 1), '--addr', '127.0.0.1', '--port', str(a.port + i),
           '--storage', str(roots[i]), '--quota', '4G', '--direct-io']
    if i:
        cmd += ['--join', f'127.0.0.1:{a.port}']
    procs[i] = subprocess.Popen(cmd, env=env, stdout=f, stderr=f)
    wait(lambda: procs[i].poll() is None and listening(a.port + i), f'n{i+1} listening', 600)


def mgmt(*args, check=True):
    return subprocess.run([str(src / 'efs-mgmt'), *args], env=env, capture_output=True, text=True, timeout=60, check=check).stdout


def gc_status(i):
    out = mgmt('gc-status', f'127.0.0.1:{a.port+i}', check=False)
    vals = {}
    for k, v in re.findall(r'(\w+)=(\w+)', out):
        vals.setdefault(k, v)
    return vals


def mount_fs():
    global client
    f = open(base / 'fuse.log', 'ab'); logs.append(f)
    client = subprocess.Popen([str(src / 'efs-fuse'), f'127.0.0.1:{a.port}', str(mount), '-f'], env=env, stdout=f, stderr=f)
    wait(lambda: subprocess.run(['mountpoint', '-q', str(mount)]).returncode == 0, 'mount', 60)


def umount_fs():
    if client and client.poll() is None:
        subprocess.run(['fusermount3', '-u', str(mount)], check=False)
        try:
            client.wait(timeout=30)
        except subprocess.TimeoutExpired:
            client.kill(); client.wait()


def content(name, size):
    seed = hashlib.sha256(name.encode()).digest()
    return (seed * (size // 32 + 1))[:size]


def frag_files(ino):
    seg = []
    for _ in range(5):
        seg.append(f'{ino % 10000:04d}'); ino //= 10000
    found = 0
    for r in roots:
        d = r / 'data/exports/1'
        for part in reversed(seg):
            d /= part
        if d.exists():
            found += sum(1 for x in d.glob('*/*') if x.is_file())
    return found


verdict = 'FAIL'
try:
    for i in range(3):
        start(i)
    time.sleep(3)
    mgmt('raft-mkfs', f'127.0.0.1:{a.port}')
    mount_fs()
    log('cluster up')
    keep, doom = [], []
    for kind, n, sizes in (('keep', a.files, (256, 4096, 16384)), ('doom', a.files, (256, 4096, 16384)),
                           ('big', a.big, (4 << 20,))):
        d = mount / kind
        d.mkdir()
        for k in range(n):
            path = d / f'{kind}-{k:06d}'
            size = sizes[k % len(sizes)]
            with open(path, 'wb') as f:
                f.write(content(path.name, size))
                os.fsync(f.fileno())
            (keep if kind == 'keep' else doom).append((path, size, os.stat(path).st_ino))
    log(f'created keep={len(keep)} doom={len(doom)}')
    assert all(frag_files(ino) for _, _, ino in doom[:50]), 'fragment layout probe found nothing'
    t0 = time.time()
    for path, _, _ in doom:
        os.unlink(path)
    log(f'unlinked {len(doom)} in {time.time()-t0:.1f}s')
    for r in range(a.rounds):
        time.sleep(rng.uniform(0.1, 2.0))
        i = rng.randrange(3)
        procs[i].send_signal(signal.SIGKILL); procs[i].wait()
        log(f'round {r+1}: SIGKILL n{i+1}')
        time.sleep(rng.uniform(0, 1.0))
        start(i)
        st = gc_status(i)
        log(f'round {r+1}: n{i+1} back, reclaim_state={st.get("reclaim_state")} journal_records={st.get("journal_records")}')
    def drained():
        sts = [gc_status(i) for i in range(3)]
        if any(s.get('reclaim_state') != 'ok' or s.get('journal_records') != '0' for s in sts):
            return False
        return all(frag_files(ino) == 0 for _, _, ino in doom)
    t0 = time.time()
    wait(drained, 'reclaim drain', a.drain_seconds)
    log(f'drained {time.time()-t0:.1f}s after the last restart')
    bad = 0
    for path, size, _ in keep:
        with open(path, 'rb') as f:
            if f.read() != content(path.name, size):
                bad += 1
    assert not bad, f'{bad} keep files differ'
    log(f'keep files exact: {len(keep)}')
    left = [ino for _, _, ino in keep if not frag_files(ino)]
    assert not left, f'{len(left)} keep inodes lost their fragments'
    before = [int(gc_status(i)['data_usage_bytes']) for i in range(3)]
    umount_fs()
    for i in range(3):
        procs[i].send_signal(signal.SIGTERM); procs[i].wait(timeout=60)
    for i in range(3):
        start(i)
    after = [int(gc_status(i)['data_usage_bytes']) for i in range(3)]
    log(f'usage before restart {before} after {after}')
    assert before == after, 'quota accounting differs from the startup inventory'
    verdict = 'PASS'
finally:
    umount_fs()
    for pr in procs:
        if pr and pr.poll() is None:
            pr.send_signal(signal.SIGTERM)
            try:
                pr.wait(timeout=60)
            except subprocess.TimeoutExpired:
                pr.kill()
    log('reclaim crash gate: ' + verdict)
sys.exit(0 if verdict == 'PASS' else 1)
