"""K33 test helper: private efsd daemons on loopback with the positional CLI.

Used by tests/test_w153_formation.py, tests/test_k33_growth.py and
tests/test_k33_autoid.py. Every daemon gets a free loopback port (never
17xxx), a store under the test's work directory and a discovery port that
is 0 (off) unless the test names a private one; discovery probes only `lo`.
The seed starts as `efsd ADDR:PORT STORAGE`; a joiner as `efsd --join
[SEED] --listen ADDR:PORT STORAGE`. Ports are handed out in increasing
order, so the formation proposal (endpoint order) gives node ids that match
the logical numbers of the nodes that formed, and an addition started on its
own gets the next id.

The daemon is tests/faults/efsd (`make tests/faults/efsd`): every server
source built with EFS_FAULTS, so the tests can interrupt a handoff
(EFS_FAULT_MEMBER_EXIT) and stop GC (EFS_FAULT_GC_DISABLE). ./efsd carries
neither hook.
"""
import os, re, signal, socket, subprocess, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
EFSD, MGMT, FUSE = root / 'tests/faults/efsd', root / 'efs-mgmt', root / 'efs-fuse'
HOST = '127.0.0.1'


def free_port(host=HOST, udp=False):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM) as s:
        s.bind((host, 0))
        return s.getsockname()[1]


def free_ports(n):
    ports = set()
    while len(ports) < n:
        ports.add(free_port())
    return sorted(ports)


def mgmt(env, *args):
    p = subprocess.run([str(MGMT), *args], capture_output=True, text=True, env=env, timeout=60)
    return p.returncode, p.stdout + p.stderr


class Cluster:
    def __init__(self, work, name, ids, env, disc_port=0, hosts=None):
        self.dir = work / name
        self.dir.mkdir()
        self.env = env
        self.disc_port = disc_port
        self.hosts = hosts or {}
        self.ports = dict(zip(ids, free_ports(len(ids))))
        self.procs = {}

    def ep(self, i):
        return f'{self.hosts.get(i, HOST)}:{self.ports[i]}'

    def start(self, i, join=None, store=None, extra_env=None, listen=True, args=(), quota='1G'):
        """join: None (seed / restart by endpoint), a node number, 'discover'
        (no SEED: local-subnet discovery on lo) or an ADDR:PORT string."""
        store = self.dir / (store or f'n{i}')
        store.mkdir(exist_ok=True)
        if join is None:
            argv = [str(EFSD), self.ep(i), str(store)]
        else:
            argv = [str(EFSD), '--join']
            if join == 'discover':
                argv += ['--discover-on', 'lo']
            else:
                argv += [self.ep(join) if isinstance(join, int) else join]
            if listen:
                argv += ['--listen', self.ep(i)]
            argv += [str(store)]
        if quota:
            argv += ['--quota', quota]
        argv += ['--writers', '2', '--discovery-port', str(self.disc_port), *args]
        with open(self.dir / f'n{i}.log', 'a') as log:
            log.write(f'\n==== start {time.time():.3f} {" ".join(argv)}\n')
            log.flush()
            self.procs[i] = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
                                             env=dict(self.env, **(extra_env or {})))
        return self.procs[i]

    def log(self, i):
        return (self.dir / f'n{i}.log').read_text(errors='replace')

    def last_run(self, i):
        return self.log(i).rsplit('==== start', 1)[-1]

    def wait_log(self, i, pattern, timeout=30, run=True):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.last_run(i) if run else self.log(i)):
                return
            if i in self.procs and self.procs[i].poll() is not None and run:
                if re.search(pattern, self.last_run(i)):
                    return
                break
            time.sleep(0.05)
        raise AssertionError(f'node {i}: {pattern!r} not seen\n{self.last_run(i)[-3000:]}\n' +
                             self.others(i))

    def others(self, i):
        """State and log tail of every other daemon, for a failure message."""
        out = []
        for k in sorted(self.ports):
            if k == i or not (self.dir / f'n{k}.log').exists():
                continue
            p = self.procs.get(k)
            st = 'not running' if p is None else ('running' if p.poll() is None
                                                  else f'exited {p.returncode}')
            tail = self.last_run(k).strip().splitlines()[-4:]
            out.append(f'-- node {k} ({st}):\n' + '\n'.join(t[:300] for t in tail))
        return '\n'.join(out)

    def wait_exit(self, i, timeout=30):
        rc = self.procs[i].wait(timeout=timeout)
        self.procs.pop(i)
        return rc

    def alive(self, i):
        return i in self.procs and self.procs[i].poll() is None

    def node_id(self, i):
        """The node id the cluster gave the daemon started as logical node i."""
        m = re.findall(r'node (\d+) listening on ', self.last_run(i))
        assert m, self.last_run(i)[-2000:]
        return int(m[-1])

    def cluster_id(self, i):
        m = re.findall(r'cluster ([0-9a-f]{16})', self.last_run(i))
        assert m, self.last_run(i)[-2000:]
        return m[-1]

    def stop(self, i, sig=signal.SIGTERM):
        p = self.procs.pop(i, None)
        if p and p.poll() is None:
            p.send_signal(sig)
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()

    def stop_all(self):
        for i in list(self.procs):
            self.stop(i)

    def status(self, i):
        out = ''
        for _ in range(3):
            rc, out = mgmt(self.env, 'raft-status', self.ep(i))
            if rc == 0:
                # (serves, voters): a retired replica (K33 M6) does not serve.
                return {int(g): (int(h) and not int(r), int(v, 16)) for g, h, v, r in
                        re.findall(r'group (\d+) hosted=(\d+) .*?voters=(0x[0-9a-f]+) joint=\d+ '
                                   r'retired=(\d+)', out)}
            time.sleep(0.5)
        raise AssertionError(out)

    def wait_voters(self, ids, want, timeout=60):
        end, got = time.time() + timeout, {}
        while time.time() < end:
            got = {i: self.status(i) for i in ids}
            if all(want(i, got[i]) for i in ids):
                return got
            time.sleep(0.3)
        raise AssertionError(f'voters never settled: {got}\n' +
                             '\n'.join(self.last_run(i)[-1500:] for i in ids))

    def restart(self, i, extra_env=None, quota='1G'):
        """Start logical node i again from its store: the seed store by its
        endpoint, a joined store with --join through a live member."""
        seed = None if i == 1 else next((k for k in sorted(self.procs)
                                          if k != i and self.alive(k)), 1)
        return self.start(i, join=seed, extra_env=extra_env, quota=quota)

    def wait_any_exit(self, ids, timeout=60):
        """The first of ids whose daemon ends; returns (node, status)."""
        end = time.time() + timeout
        while time.time() < end:
            for i in ids:
                if i in self.procs and self.procs[i].poll() is not None:
                    return i, self.wait_exit(i)
            time.sleep(0.05)
        raise AssertionError(f'none of {ids} ended\n' +
                             '\n'.join(self.last_run(i)[-1500:] for i in ids))

    def members(self, i):
        """{node id: (endpoint, 'up'|'down')} from `efs-mgmt status` on node i."""
        rc, out = mgmt(self.env, 'status', self.ep(i))
        # status exits non-zero while a member is down; the table still counts.
        assert re.search(r'Cluster nodes \(\d+\)', out), out
        n = int(re.search(r'Cluster nodes \((\d+)\)', out).group(1))
        rows = re.findall(r'^\s+node (\d+): (\S+)\s+(up|DOWN)\b', out, re.M)
        assert len(rows) == n, out
        return {int(k): (ep, st.lower()) for k, ep, st in rows}

    def leader_placement(self, i, timeout=30):
        """A9/D42: {group: leader node id} from the placement section of
        `efs-mgmt status` on node i, once every member reported and each
        group has exactly one self-reported leader."""
        end, out = time.time() + timeout, ''
        while time.time() < end:
            rc, out = mgmt(self.env, 'status', self.ep(i))
            m = re.search(r'leaders: (\d+)/(\d+) groups on (\d+) distinct node\(s\); '
                          r'(\d+)/(\d+) members reported', out)
            if m and m.group(1) == m.group(2) and m.group(4) == m.group(5):
                led = {}
                for nid, rest in re.findall(r'^\s+node (\d+): leads (.*)$', out, re.M):
                    for g in re.findall(r'group (\d+) \(term \d+\)', rest):
                        assert int(g) not in led, out
                        led[int(g)] = int(nid)
                assert len(led) == int(m.group(2)) and len(set(led.values())) == int(m.group(3)), out
                return led
            time.sleep(0.5)
        raise AssertionError('leader placement never settled:\n' + out)

    def wait_members(self, ids, count, timeout=60):
        """Every node in ids reports the same `count` members: ids 1..count,
        distinct endpoints (no duplicate identity). Returns the table."""
        end, last = time.time() + timeout, {}
        while time.time() < end:
            try:
                last = {i: self.members(i) for i in ids}
            except AssertionError as e:
                last = {'error': str(e)}
                time.sleep(0.3)
                continue
            tabs = [{k: v[0] for k, v in t.items()} for t in last.values()]
            if all(t == tabs[0] for t in tabs) and sorted(tabs[0]) == list(range(1, count + 1)):
                assert len(set(tabs[0].values())) == count, tabs[0]
                return tabs[0]
            time.sleep(0.3)
        raise AssertionError(f'members never settled at {count}: {last}')


def voters_ok(nid, v, count):
    """raft-status of member nid (`Cluster.status`) under `count` members:
    group 0 votes on {1,2,3}; group 2 on {1,2,3} at three and {2,3,4} from
    four, served exactly by its voters. Node 1's group-2 replica is retired
    from four members: it never serves and may never learn the COLD that
    removed it (protocol M6), so its own voter view is not checked."""
    if count >= 4 and nid == 1:
        return v[0] == (1, 7) and v[2][0] == 0
    g2 = 0xe if count >= 4 else 7
    return (v[0][1] == 7 and (v[0][0] == 1) == (nid <= 3) and v[2][1] == g2 and
            (v[2][0] == 1) == (nid in ((2, 3, 4) if count >= 4 else (1, 2, 3))))


def fuse_ok():
    import shutil
    return FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and \
        bool(shutil.which('fusermount3'))


class Mount:
    def __init__(self, c, node, name, env=None):
        self.path = c.dir / name
        self.path.mkdir()
        self.logp = c.dir / f'{name}.log'
        self.log = open(self.logp, 'w')
        self.p = subprocess.Popen([str(FUSE), c.ep(node), str(self.path), '-f'],
                                  stdout=self.log, stderr=subprocess.STDOUT,
                                  env=env or c.env)
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(self.path):
            if self.p.poll() is not None:
                raise AssertionError(self.logp.read_text()[-2000:])
            time.sleep(0.1)
        assert os.path.ismount(self.path), self.logp.read_text()[-2000:]

    def close(self):
        subprocess.run(['fusermount3', '-u', str(self.path)], capture_output=True)
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            subprocess.run(['fusermount3', '-uz', str(self.path)], capture_output=True)
            self.p.kill()
            self.p.wait()


def payload(name, size):
    import hashlib
    out, k = bytearray(), 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return bytes(out[:size])


def fragments(store):
    """Fragment files (<chunk>.<fragment>.<generation>) under one store,
    metadata (Raft, KV) excluded."""
    n = 0
    for dirpath, dirnames, files in os.walk(store):
        if '/mdraft/' in dirpath + '/' or '/meta/' in dirpath + '/':
            continue
        n += sum(1 for f in files if re.match(r'^\d+\.\d+\.\d+$', f))
    return n
