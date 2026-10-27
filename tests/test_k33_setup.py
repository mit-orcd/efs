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


for mode in ('catchup', 'unknown', 'stuck'):
    c, calls, now = Cluster(), [], [0.0]

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
            assert k.place_leaders(c, 1) == {0: 1, 2: 1}
            assert len(calls) == 3 and 0 < now[0] < 1
        else:
            try:
                k.place_leaders(c, 1)
            except AssertionError as e:
                if mode == 'unknown':
                    assert len(calls) == 1 and 'storage error' in str(e) and now[0] == 0
                else:
                    assert 30 <= now[0] <= 30.2 and 'stayed BUSY' in str(e)
            else:
                raise AssertionError(f'{mode}: setup falsely succeeded')
print('K33 pressure setup: retry documented catch-up BUSY only, 30 s bound, hard errors fail PASS')
