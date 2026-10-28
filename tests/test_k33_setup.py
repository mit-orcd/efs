#!/usr/bin/env python3
"""Execute the private-cluster setup helper: catch-up BUSY is bounded,
unknown management errors fail, and no fault/application work is retried.
"""
import sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as k


class Cluster:
    ports = {1: 0, 2: 0, 3: 0}
    env = {}

    def __init__(self):
        self.leaders = {0: 2, 2: 2}

    def node_id(self, i):
        return i

    def ep(self, i):
        return f'node{i}'

    def leader_placement(self, i):
        return dict(self.leaders)


for mode, target, bound in (('catchup', 1, 30), ('catchup', 2, 20),
                            ('unknown', 1, 30), ('stuck', 1, 30), ('stuck', 2, 20)):
    c, calls, now = Cluster(), [], [0.0]
    c.leaders = {0: 3 - target, 2: 3 - target}

    def mgmt(env, *args):
        calls.append(args)
        if mode == 'unknown':
            return 1, 'rc=-1 storage error'
        if mode == 'stuck' or len(calls) == 1:
            return 1, 'rc=-13 (target not caught up or group frozen; retry)'
        c.leaders[int(args[2])] = int(args[3])
        return 0, 'rc=0'

    def sleep(seconds):
        now[0] += seconds

    with patch.object(k, 'mgmt', mgmt), patch.object(k, 'SLOW', 1), \
         patch.object(k.time, 'monotonic', lambda: now[0]), patch.object(k.time, 'sleep', sleep):
        if mode == 'catchup':
            assert k.place_leaders(c, target, timeout=bound) == {0: target, 2: target}
            assert len(calls) == 3 and 0 < now[0] < 1
        else:
            try:
                k.place_leaders(c, target, timeout=bound)
            except AssertionError as e:
                if mode == 'unknown':
                    assert len(calls) == 1 and 'storage error' in str(e) and now[0] == 0
                else:
                    assert bound <= now[0] <= bound + .2 and 'stayed BUSY' in str(e)
            else:
                raise AssertionError(f'{mode}: setup falsely succeeded')
print('K33 pressure setup: retry documented catch-up BUSY only, 30 s bound, hard errors fail PASS')

# Join completion may assign member 3 to the process named node2. The
# requested target and the management endpoint must use assigned identities.
c = Cluster()
c.node_id = lambda i: {1: 1, 2: 3, 3: 2}[i]
c.leaders = {0: 1, 2: 1}
calls = []
def assigned_mgmt(env, *args):
    calls.append(args)
    c.leaders[int(args[2])] = int(args[3])
    return 0, 'rc=0'
with patch.object(k, 'mgmt', assigned_mgmt):
    assert k.place_leaders(c, 2, timeout=20) == {0: 3, 2: 3}
assert all(a[1] == 'node1' and a[3] == '3' for a in calls), calls
print('K33 pressure setup: assigned target identity, independent of process start order PASS')

# A failed mount constructor cannot put itself in the caller's cleanup list.
# Preserve diagnostics but stop its process on immediate exit and timeout.
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock
for failed_exit in (False, True):
    with tempfile.TemporaryDirectory(prefix='efs-mount-setup-') as directory:
        c = SimpleNamespace(dir=Path(directory), env={}, ep=lambda i: '127.0.0.1:19999')
        process = Mock()
        process.poll.return_value = 1 if failed_exit else None
        process.wait.return_value = 0
        with patch.object(k.subprocess, 'Popen', return_value=process), \
             patch.object(k.subprocess, 'run'), patch.object(k.os.path, 'ismount', return_value=False), \
             patch.object(k.time, 'time', side_effect=[0, 0 if failed_exit else 31]):
            try:
                k.Mount(c, 1, 'mount')
            except AssertionError:
                pass
            else:
                raise AssertionError('failed mount falsely succeeded')
        process.wait.assert_called_once()
        assert (c.dir / 'mount.log').exists()
print('K33 failed mount constructor: child joined, fixture/log retained on exit and timeout PASS')
