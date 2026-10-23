"""D39 helpers for live tests that exercise one efsd directly.

A daemon serves clients only once its cluster formation is complete, and
formation (raft-mkfs) needs every configured member present (D39/D40). A
test that drives one daemon over the wire therefore runs it as member 1 of a
three-member configured list with two support members, forms the cluster
once, and points efs-mgmt at the list (EFS_CLUSTER_CONF) so it declares as
management.
"""
import os
from pathlib import Path
import socket
import subprocess
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def write_conf(path, ports, addr='127.0.0.1'):
    Path(path).write_text(''.join('%d %s:%d\n' % (i + 1, addr, p) for i, p in enumerate(ports)))
    return Path(path)


def wait_listen(proc, port, what='efsd', seconds=30):
    end = time.monotonic() + seconds
    while True:
        assert proc.poll() is None, '%s on %d exited' % (what, port)
        try:
            with socket.create_connection(('127.0.0.1', port), .2):
                return
        except OSError:
            assert time.monotonic() < end, '%s on %d did not listen' % (what, port)
            time.sleep(.1)


class Support:
    """Members 2 and 3 of a three-member list whose member 1 the test runs."""

    def __init__(self, source, work, member1_port, env=None, quota='16M'):
        self.source, self.work = Path(source), Path(work)
        self.ports = [member1_port, free_port(), free_port()]
        self.conf = write_conf(self.work / 'cluster.conf', self.ports)
        self.env = dict(env or os.environ, EFS_CLUSTER_CONF=str(self.conf))
        self.quota, self.procs, self.logs = quota, [], []

    def start(self):
        for i in (1, 2):
            store = self.work / ('support%d' % (i + 1))
            store.mkdir(exist_ok=True)
            log = (self.work / ('support%d.log' % (i + 1))).open('a')
            self.logs.append(log)
            p = subprocess.Popen([str(self.source / 'efsd'), '--cluster', str(self.conf), '--addr', '127.0.0.1',
                                  '--port', str(self.ports[i]), '--storage', str(store), '--quota', self.quota],
                                 env=self.env, stdout=log, stderr=log)
            self.procs.append(p)
            wait_listen(p, self.ports[i], 'support member %d' % (i + 1))

    def form(self, seconds=90):
        r = subprocess.run([str(self.source / 'efs-mgmt'), '--cluster', str(self.conf), 'raft-mkfs',
                            '127.0.0.1:%d' % self.ports[0], '--wait', str(seconds)],
                           env=self.env, capture_output=True, text=True, timeout=seconds + 30)
        assert r.returncode == 0, ('raft-mkfs', r.stdout, r.stderr)

    def stop(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(10)
        for log in self.logs:
            log.close()
        self.procs, self.logs = [], []
