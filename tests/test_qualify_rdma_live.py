#!/usr/bin/env python3
"""Live qualification gates select RDMA from the cluster description (Task BS).

Hardware-free: the driver's gate is run against a fake runner. The RDMA runs on
a real HCA are NOTRUN here; see docs/status/rdma-qualification-driver-20261031.md.
"""
import importlib.util
import subprocess
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('qualify', root / 'scripts/qualify.py')
Q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(Q)


class G:
    def __init__(self, device_rc=0):
        self.cmds, self.fixture, self.commands, self.device_rc = [], {}, [{}], device_rc

    def run(self, label, host, cmd, deadline=None):
        self.cmds.append((label, cmd))
        return (self.device_rc if label == 'rdma-device' else 0), 'ok'


def run(build, cluster, device_rc=0):
    q = SimpleNamespace(cluster=cluster, fixture=dict(Q.DEFAULT_FIXTURE),
                        private=dict(host='h', root='/r', port_base=18700),
                        source=lambda host: '/src', runid='x')
    g = G(device_rc)
    g.gate = SimpleNamespace(name='t')
    live_dir = Q.live_dir
    Q.live_dir = lambda q, g: '/w'
    try:
        verdict = Q.live_gate(build, rdma=True)(q, g)
    finally:
        Q.live_dir = live_dir
    return verdict, g


for build in (Q.b_append, Q.b_k14, Q.b_gc, Q.b_ao):
    (verdict, _), g = run(build, dict(transport='tcp'))
    text = ' '.join(c for _, c in g.cmds)
    assert verdict == Q.PASS and g.fixture['transport'] == 'tcp', build
    assert 'rdma' not in text.lower() and 'rdma_dev' not in g.fixture, (build, text)
    (verdict, _), g = run(build, dict(transport='rdma', rdma_dev='mlx5_0'))
    text = ' '.join(c for _, c in g.cmds)
    assert verdict == Q.PASS and g.fixture['transport'] == 'rdma' and g.fixture['rdma_dev'] == 'mlx5_0', build
    assert 'export EFS_RDMA_DEV=mlx5_0 &&' in text and ('rdma' in text.replace('EFS_RDMA_DEV', '')), (build, text)
    # no device named / device absent is NOTRUN, never a silent TCP run
    assert run(build, dict(transport='rdma'))[0][0] == Q.NOTRUN
    assert run(build, dict(transport='rdma', rdma_dev='mlx5_0'), device_rc=1)[0][0] == Q.NOTRUN

# harnesses that do not take a transport stay TCP and are labelled so, even on an RDMA cluster
q = SimpleNamespace(cluster=dict(transport='rdma', rdma_dev='mlx5_0'), fixture=dict(Q.DEFAULT_FIXTURE),
                    private=dict(host='h', root='/r', port_base=18700), source=lambda h: '/src', runid='x')
g = G(); g.gate = SimpleNamespace(name='t')
Q.live_dir, saved = (lambda q, g: '/w'), Q.live_dir
try:
    assert Q.live_gate(Q.b_k13)(q, g)[0] == Q.PASS
finally:
    Q.live_dir = saved
assert g.fixture['transport'] == 'tcp' and 'EFS_RDMA_DEV' not in ' '.join(c for _, c in g.cmds)

assert 'ao-readdir' in {x.name for x in Q.gates(Q.default_plugins({}, []))}
for script in ('scripts/k14_atomic_cluster.sh',):
    assert subprocess.run(['bash', '-n', str(root / script)]).returncode == 0, script
print('PASS test_qualify_rdma_live (RDMA hardware runs NOTRUN here)')
