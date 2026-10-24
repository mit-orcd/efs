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
"""
import os, re, signal, socket, subprocess, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
EFSD, MGMT, FUSE = root / 'efsd', root / 'efs-mgmt', root / 'efs-fuse'
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

    def start(self, i, join=None, store=None, extra_env=None, listen=True, args=()):
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
        argv += ['--quota', '1G', '--writers', '2', '--discovery-port', str(self.disc_port), *args]
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
        raise AssertionError(f'node {i}: {pattern!r} not seen\n{self.last_run(i)[-3000:]}')

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
