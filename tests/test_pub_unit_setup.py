#!/usr/bin/env python3
"""Transfer setup waits for documented readiness; never hides other errors."""
from pub_unit_setup import split_leaders, leaders_on


class Fixture:
    env = {}
    def __init__(self):
        self.now = 0
        self.separate = False
        self.calls = 0
    def leader_placement(self, node):
        return {0: 3, 2: 1 if self.separate else 3}
    def node_id(self, node):
        return node
    def ep(self, node):
        return f'127.0.0.1:{19000+node}'
    def sleep(self, duration):
        self.now += duration
    def run(self, mgmt):
        return split_leaders(self, mgmt, 2, clock=lambda: self.now, sleep=self.sleep)


for transient in (-13, -15):
    f = Fixture()
    def mgmt(*args):
        f.calls += 1
        if f.calls == 1:
            return 1, f'raft-step-down rc={transient}'
        f.separate = True
        return 0, 'raft-step-down rc=0'
    assert f.run(mgmt) == {0: 3, 2: 1} and f.calls == 2

f = Fixture()
try:
    f.run(lambda *a: (1, 'raft-step-down rc=-7'))
except AssertionError as e:
    assert 'rc=-7' in str(e)
else:
    raise AssertionError('fatal protocol error was ignored')
f = Fixture()
try:
    f.run(lambda *a: (1, 'raft-step-down rc=-13'))
except AssertionError as e:
    assert 'did not separate' in str(e) and f.now == 2
else:
    raise AssertionError('permanent BUSY did not fail at the setup deadline')
f = Fixture()
f.separate = True
assert f.run(lambda *a: (_ for _ in ()).throw(AssertionError('unneeded transfer'))) == {0: 3, 2: 1}
print('publication topology setup: transient catch-up, leadership change, fatal error and deadline PASS')


class TargetFixture(Fixture):
    ports = {1: 19001, 2: 19002, 3: 19003}
    def __init__(self):
        super().__init__()
        self.place = {0: 1, 2: 3}
    def leader_placement(self, node):
        return dict(self.place)
    def run(self, mgmt):
        return leaders_on(self, mgmt, 2, 2, clock=lambda: self.now, sleep=self.sleep)


for transient in (-13, -15):
    f = TargetFixture()
    def mgmt(*args):
        f.calls += 1
        assert args[-1] == '2' and args[-3] in (f.ep(1), f.ep(3))
        if f.calls == 1:
            return 1, f'raft-step-down rc={transient}'
        f.place[int(args[-2])] = 2
        return 0, 'raft-step-down rc=0'
    assert f.run(mgmt) == {0: 2, 2: 2} and f.calls == 3
for reply in ('rc=-7', 'no protocol status'):
    f = TargetFixture()
    try:
        f.run(lambda *a: (1, reply))
    except AssertionError as error:
        assert reply in str(error)
    else:
        raise AssertionError('fatal transfer error ignored')
f = TargetFixture()
try:
    f.run(lambda *a: (1, 'rc=-13'))
except AssertionError:
    assert f.now == 2 and f.place == {0: 1, 2: 3}
else:
    raise AssertionError('permanent catch-up BUSY did not stop at the setup deadline')
f.place = {0: 2, 2: 2}
assert f.run(lambda *a: (_ for _ in ()).throw(AssertionError('unneeded transfer'))) == f.place
print('stopping-leader setup: both groups verified, bounded readiness, fatal replies preserved PASS')
