#!/usr/bin/env python3
"""Run the unchanged task-I D4 test on owned 19xxx endpoints.

Only the test's endpoint allocator is adapted; all workload sizes, deadlines,
assertions and cleanup are supplied by tests/test_d4_plateau.py unchanged.
A non-loopback address is necessary for a same-host RDMA run.
"""
import argparse
import ipaddress
import os
from pathlib import Path
import runpy
import socket
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host-address', default='127.0.0.1')
p.add_argument('--allocator-start-marker', type=Path, help='diagnostic-only observer synchronization')
a = p.parse_args()
address = ipaddress.IPv4Address(a.host_address)
if os.environ.get('EFS_TRANSPORT') == 'rdma' and address.is_loopback:
    p.error('RDMA needs the assigned host\'s HCA IPv4 address, not loopback')
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests'))
import k33_daemons as h  # noqa: E402

candidate = 19000

def private_port(host=None, udp=False):
    global candidate
    for _ in range(1000):
        candidate = 19000 + (candidate - 19000 + 1) % 1000
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM) as s:
            try:
                s.bind((str(address), candidate))
            except OSError:
                continue
            return candidate
    raise RuntimeError('no private 19xxx port available')

if a.allocator_start_marker:
    sys.path.insert(0, str(root / 'scripts'))
    import d4_progress
    original_sample = d4_progress.Run.sample
    def diagnostic_sample(self, phase):
        if phase == 'before':
            # This callback runs after population and before writers start.
            with a.allocator_start_marker.open('x') as marker:
                marker.write(str(self.t0) + '\n')
        return original_sample(self, phase)
    d4_progress.Run.sample = diagnostic_sample

original_init = h.Cluster.__init__
def cluster_init(self, work, name, ids, env, disc_port=0, hosts=None):
    original_init(self, work, name, ids, env, disc_port,
                  hosts or {i: str(address) for i in ids})

h.free_port = private_port
h.Cluster.__init__ = cluster_init
sys.argv = [str(root / 'tests/test_d4_plateau.py')]
runpy.run_path(sys.argv[0], run_name='__main__')
