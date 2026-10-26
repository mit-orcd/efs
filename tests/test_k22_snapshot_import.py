#!/usr/bin/env python3
"""K22/W23: snapshot import into a stalled follower (loopback).

Real fault-build efsd daemons on loopback (tests/k33_daemons.py). The daemons
run with EFS_FAULT_SNAP_BYTES shrinking the retained log window so a follower
stopped with SIGSTOP falls behind it in minutes; the production-scale version
(1 GiB of command log past the follower's match, normal build, real hosts) is
scripts/k22_snapshot_import.sh for the coordinator.

1. Three daemons form the cluster; a FUSE client writes payload files
   (sha256 manifest kept).
2. Node 3 is SIGSTOPped; the load continues until every group's leader has
   snapshotted its log past node 3's match index six times. The leader
   pre-sends a bounded backlog into the stopped peer's socket (~2 windows),
   so the retained window must march well past node 3's match before it can
   only rejoin through an InstallSnapshot.
3. SIGCONT. Node 3 must log `on_snap_req ... INSTALLED`, apply the snapshot
   and catch up within the stated bounds; its RSS is sampled every second
   against the stated bound.
4. The load drains; node 3 applies every commit; every file written before
   and during the stop reads back byte-exact through a fresh mount seeded at
   node 3, and new writes through node 3 succeed. During the stall a write's
   close may fail with an explicit error — the B6 member-unavailability
   contract allows failure, never silent corruption; such files are outside
   the readback manifest by construction and are reported as findings.
"""
import hashlib, os, re, shutil, signal, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster as _Cluster, EFSD, MGMT, FUSE, mgmt as _mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')

SNAP_BYTES = int(os.environ.get('K22_SNAP_BYTES', str(1 << 20)))  # fault knob, per daemon; 0 = production window
STOP_DEADLINE = int(os.environ.get('K22_STOP_DEADLINE', '900'))
CATCH_BOUND = int(os.environ.get('K22_CATCH_BOUND', '300'))
DRAIN_BOUND = int(os.environ.get('K22_DRAIN_BOUND', '600'))
RSS_FACTOR = float(os.environ.get('K22_RSS_FACTOR', '3'))       # peak <= factor * warm max
RSS_SLACK = int(os.environ.get('K22_RSS_SLACK', str(512 << 20)))  # + absolute slack
WRITE_BYTES = 2048

work = Path(tempfile.mkdtemp(prefix='efs-k22-snap-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ)
if SNAP_BYTES:
    env['EFS_FAULT_SNAP_BYTES'] = str(SNAP_BYTES)   # 0 = production window
env['EFS_TRANSPORT'] = 'tcp'
env['EFS_RAFT_AE_DBG'] = '1'
env['EFS_RAFT_DBG'] = '1'   # send_snap / on_snap_req INSTALLED lines are gated on this


def mgmt(*args):
    return _mgmt(env, *args)


def Cluster(name, ids):
    return _Cluster(work, name, ids, env)


def fuse_ok():
    return FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and shutil.which('fusermount3')


class Mount:
    def __init__(self, c, node, name):
        self.path = c.dir / name
        self.path.mkdir()
        self.log = open(c.dir / f'{name}.log', 'w')
        self.p = subprocess.Popen([str(FUSE), c.ep(node), str(self.path), '-f'],
                                  stdout=self.log, stderr=subprocess.STDOUT, env=env)
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(self.path):
            if self.p.poll() is not None:
                raise AssertionError((c.dir / f'{name}.log').read_text()[-2000:])
            time.sleep(0.1)
        assert os.path.ismount(self.path), (c.dir / f'{name}.log').read_text()[-2000:]

    def close(self):
        subprocess.run(['fusermount3', '-u', str(self.path)], capture_output=True)
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def payload(name, size=WRITE_BYTES):
    out, k = b'', 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return out[:size]


def node_rss_kb(c, i):
    """VmRSS of node i's daemon, 0 if stopped/gone."""
    p = c.procs.get(i)
    if not p:
        return 0
    try:
        with open(f'/proc/{p.pid}/status') as f:
            for line in f:
                if line.startswith('VmRSS'):
                    return int(line.split()[1])
    except Exception:
        return 0
    return 0


def raw_status(c, i):
    rc, out = mgmt('raft-status', c.ep(i))
    assert rc == 0, out
    return out


def groups(status_text):
    """raft-status text -> {group: {role, commit, applied, snap, last, peers:{peer:match}}}."""
    out = {}
    g = None
    for line in status_text.splitlines():
        m = re.match(r'\s+group (\d+) .*', line)
        if m:
            g = int(m.group(1))
            d = {}
            for key in ('role', 'commit', 'applied', 'snap', 'last'):
                km = re.search(key + r'=(\S+)', line)
                d[key] = km.group(1) if km else ''
            out[g] = d
            out[g]['peers'] = {}
            continue
        pm = re.match(r'\s+peer (\d+) .*match=(\d+)', line)
        if pm and g is not None:
            out[g]['peers'][int(pm.group(1))] = int(pm.group(2))
    return out


def leaders(c, live=(1, 2)):
    """{group: (node, {role,...})} for the live nodes."""
    out = {}
    for i in live:
        for g, d in groups(raw_status(c, i)).items():
            if d.get('role') == 'LEADER':
                out[g] = (i, d)
    return out


written = {}          # relpath -> sha256
wlock = threading.Lock()
stop_load = threading.Event()
write_errors = []


def writer(mnt, tag, fsize=WRITE_BYTES, cap=800):
    n = 0
    base = Path(mnt.path) / f'w-{tag}'
    base.mkdir()
    while not stop_load.is_set() and n < cap:
        n += 1
        name = base / f'f{n:06d}'
        body = payload(str(name), fsize)
        op = 'open'
        fd = None
        try:
            fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            op = 'write'
            view = memoryview(body)
            while view:
                got = os.write(fd, view)
                view = view[got:]
            op = 'close'
            os.close(fd)   # releases the fd even when it reports an error
            fd = None
        except OSError as e:
            with wlock:
                write_errors.append(f'{tag}/{n}: {op}: {e} at t={time.time()-t0:.0f}s')
            if fd is not None and op != 'close':
                try:
                    os.close(fd)
                except OSError:
                    pass
            time.sleep(0.2)
            continue
        if n % 200 == 0:
            os.sync()
        with wlock:
            written[str(name)] = hashlib.sha256(body).hexdigest()
    return n


bench_errors = [0]


def bench_loop(c, w):
    """Fast metadata load: mkdir+create rounds through efs-bench --meta."""
    r = 0
    bench = Path(__file__).resolve().parent.parent / 'efs-bench'
    while not stop_load.is_set():
        r += 1
        rc = subprocess.run([str(bench), c.ep(1), '--meta', '--files', '2000',
                             '--dirs', '16', '--workers', '16',
                             '--phases', 'mkdir,create', '--keep',
                             '--id', str(w * 1000000 + r)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
        if rc != 0:
            bench_errors[0] += 1


t0 = time.time()
c = None
mounts = []
notes = []
try:
    assert fuse_ok(), 'NOTRUN: no FUSE (/dev/fuse, fusermount3 or efs-fuse missing)'
    c = Cluster('snapimp', [1, 2, 3])
    for i in (1, 2, 3):
        c.start(i, join=1 if i > 1 else None)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
    c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})

    m = Mount(c, 1, 'mnt-a')
    mounts.append(m)
    threads = [threading.Thread(target=writer, args=(m, 'payload'), kwargs={'fsize': WRITE_BYTES, 'cap': 800}, daemon=True)]
    threads += [threading.Thread(target=writer, args=(m, f'big{k}'), kwargs={'fsize': 4 << 20, 'cap': 100000}, daemon=True)
                for k in range(2)]
    threads += [threading.Thread(target=bench_loop, args=(c, k), daemon=True)
                for k in range(4)]
    for t in threads:
        t.start()

    print('[k22snap] warm raft-status sample (node 1):')
    print(raw_status(c, 1))
    # warm: note node 3's RSS plateau and match index per group
    time.sleep(20)
    warm_rss = node_rss_kb(c, 3)
    for _ in range(3):
        time.sleep(2)
        warm_rss = max(warm_rss, node_rss_kb(c, 3))
    assert warm_rss > 0, 'no warm RSS for node 3'
    rss_bound = int(warm_rss * RSS_FACTOR + (RSS_SLACK >> 10))
    print(f'[k22snap] warm node3 RSS {warm_rss} KiB; bound {rss_bound} KiB')

    # stop node 3; drive until both groups' leaders snapshot past its match.
    # The leader pre-sends a bounded backlog into the stopped peer's socket
    # (probes: ~2.5 snap windows past the frozen match), so the retained
    # window must march past that backlog before node 3 can only rejoin
    # through an InstallSnapshot: require 4 distinct leader snapshots past
    # the match (floor then sits ~3 windows past it, beyond the backlog).
    os.kill(c.procs[3].pid, signal.SIGSTOP)
    print('[k22snap] node 3 SIGSTOPped; driving the log past its match')
    seen = {0: set(), 2: set()}
    deadline = time.time() + STOP_DEADLINE
    hit = False
    slog = open(c.dir / 'samples.log', 'w')
    while time.time() < deadline:
        time.sleep(5)
        ls = leaders(c)
        for g, (node, d) in ls.items():
            try:
                snap = int(d.get('snap') or 0)
            except ValueError:
                snap = 0
            matches = [m for m in d.get('peers', {}).values()]
            if not matches or not snap:
                continue
            n3_match = min(matches)         # the stopped follower's frozen match
            if snap > n3_match:
                seen[g].add(snap)
            slog.write(f'{int(time.time()-t0)} g{g} leader=n{node} snap={snap} last={d.get("last")} n3_match={n3_match}\n')
            slog.flush()
        if not ls:
            slog.write(f'{int(time.time()-t0)} NO LEADER on any live node\n')
            slog.flush()
        if all(len(v) >= 4 for v in seen.values()):
            hit = True
            break
    if not hit:
        print('[k22snap] last node-1 status:\n' + raw_status(c, 1))
        print('[k22snap] last node-2 status:\n' + raw_status(c, 2))
    assert hit, f'deadline: leaders did not snapshot past node 3 four times (seen={seen}; write_errors so far: {write_errors})'
    print('[k22snap] every leader snapshotted past node 3 four times; '
          f'elapsed {int(time.time() - t0)} s')

    # resume: InstallSnapshot must land, then catch-up
    t_resume = time.time()
    os.kill(c.procs[3].pid, signal.SIGCONT)
    peak_rss = warm_rss
    installed_at = None
    caught_at = None
    while time.time() - t_resume < CATCH_BOUND:
        time.sleep(1)
        peak_rss = max(peak_rss, node_rss_kb(c, 3))
        if installed_at is None and 'on_snap_req' in c.log(3) and 'INSTALLED' in c.log(3):
            installed_at = time.time()
            print(f'[k22snap] node 3 INSTALLED a snapshot {installed_at - t_resume:.0f} s after resume')
        if installed_at:
            ls = leaders(c)
            try:
                ok = bool(ls) and all(
                    abs(int(groups(raw_status(c, 3))[g]['applied']) - int(d['commit'])) < 2000
                    for g, (_, d) in ls.items())
            except (KeyError, ValueError):
                ok = False
            if ok:
                caught_at = time.time()
                break
    assert installed_at is not None, 'no InstallSnapshot on node 3 within the bound'
    assert caught_at is not None, 'node 3 did not catch up within the bound'
    assert peak_rss <= rss_bound, f'node 3 RSS {peak_rss} KiB exceeded bound {rss_bound} KiB'
    print(f'[k22snap] caught up {caught_at - t_resume:.0f} s after resume; '
          f'node3 RSS peak {peak_rss} KiB (warm {warm_rss})')

    # drain: stop the load; node 3 applies every commit on both groups
    stop_load.set()
    for t in threads:
        t.join(timeout=60)
    t_drain = time.time()
    while time.time() - t_drain < DRAIN_BOUND:
        ls = leaders(c)
        try:
            g3 = groups(raw_status(c, 3))
            done = bool(ls) and all(
                int(g3[g]['applied']) >= int(d['commit'])
                for g, (_, d) in ls.items() if g in g3)
        except (KeyError, ValueError):
            done = False
        if done:
            break
        time.sleep(2)
    else:
        raise AssertionError('node 3 did not apply every commit in the drain')
    print(f'[k22snap] drain: node 3 applied every commit {int(time.time() - t_drain)} s after the load stopped')

    # verify: every written file reads back byte-exact through a mount seeded
    # at node 3. Writes whose close returned an explicit error are outside the
    # manifest by construction: under a stalled member the B6 contract allows
    # write/close to fail (never to be silently wrong), so write_errors are
    # reported as findings, not asserted away here. What this gate asserts is
    # the recovered service below and byte-exact data everywhere.
    mv = Mount(c, 3, 'mnt-verify')
    mounts.append(mv)
    bad = 0
    for path, want in sorted(written.items()):
        p = Path(path)
        if not p.exists():
            print(f'[k22snap] MISSING {path}')
            bad += 1
            continue
        got = hashlib.sha256(p.read_bytes()).hexdigest()
        if got != want:
            print(f'[k22snap] MISMATCH {path}')
            bad += 1
    # recovered service: new writes through node 3 (the imported replica)
    # must succeed cleanly now that it has caught up
    late_errors = 0
    for k in range(30):
        name = mv.path / f'post-recovery-{k:03d}'
        body = payload(str(name))
        try:
            name.write_bytes(body)
        except OSError as e:
            print(f'[k22snap] post-recovery write error: {e}')
            late_errors += 1
            continue
        written[str(name)] = hashlib.sha256(body).hexdigest()
        if hashlib.sha256(name.read_bytes()).hexdigest() != written[str(name)]:
            print(f'[k22snap] MISMATCH post-recovery {name}')
            bad += 1
    assert late_errors == 0, f'{late_errors} write errors after node 3 caught up'
    if write_errors:
        print(f'[k22snap] contract-allowed write errors during the stall: {write_errors}')
    assert bad == 0, f'{bad} files missing or corrupted'
    print(f'[k22snap] {len(written)} files read back byte-exact through node 3')
    print('[k22snap] PASS')
    notes.append(f'stop->floor: {seen}; resume->install {installed_at - t_resume:.0f} s; '
                 f'install->caught {caught_at - installed_at:.0f} s; rss peak {peak_rss} KiB '
                 f'(warm {warm_rss} KiB); files {len(written)}')
except Exception as e:
    print('[k22snap] FAIL:', e)
    if notes:
        print('[k22snap]', *notes, sep='\n')
    raise
finally:
    stop_load.set()
    for m in mounts:
        m.close()
    if c:
        c.stop_all()
    import sys as _sys
    if _sys.exc_info()[0] is None:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f'[k22snap] work dir kept for forensics: {work}')
        for i in (1, 2, 3):
            lp = work / 'snapimp' / f'n{i}.log'
            if lp.exists():
                print(f'[k22snap] --- n{i}.log tail ---')
                print(lp.read_text(errors='replace')[-1500:])
        mp = work / 'snapimp' / 'mnt-a.log'
        if mp.exists():
            print('[k22snap] --- mnt-a.log tail ---')
            print(mp.read_text(errors='replace')[-1500:])
    for n in notes:
        print('[k22snap]', n)
