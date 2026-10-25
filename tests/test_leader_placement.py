#!/usr/bin/env python3
"""A9: scripts/leader_placement.py parses fixed raft-status output and sums time.

1. parse_raft_status reads the node id, each group's role (raft-status
   prints LEADER/FOLLOWER/CANDIDATE), believed leader (raft id + 1), term,
   voters and retired flag; a non-hosted group reads as hosted=0.
2. leaders_of keeps self-reported leaders only: a retired or non-hosted
   replica never counts, the higher term wins two claims, a group nobody
   leads reads 0.
3. parse_members reads the `efs-mgmt status` member table (up and DOWN) and
   ignores the placement section's `node N: leads ...` lines.
4. summarize weights each sample until the next one: per-group shares,
   no-leader time, all-on-one-node share, leader changes (a return of the
   same leader after a leaderless gap is not a change).
5. schedule interleaves step-downs and restarts evenly.

Usage: python3 tests/test_leader_placement.py (no build, no daemons)"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import leader_placement as lp  # noqa: E402

TAIL = ('commit=3 applied=3 voters={v} joint=0 retired={r} last=3 last_term=1 snap=0 '
        'rejects_sent=0 proto_drops=0 unapplied=0 max_unapplied=524288 busy_sent=0 '
        'admit_busy=0 low_capacity=0 campaigns_held=0 capacity_stepdowns=0')


def status(node, groups):
    """raft-status text as efs-mgmt prints it (format of src/mgmt/efs_mgmt.c)."""
    out = [f'node {node} root=1 salt=6438199982882807812 groups={len(groups)}',
           '  bound kv_mt_bytes=998 kv_mem_budget=134217728 kv_l0_bytes=0 '
           'kv_l0_cap=1073741824 kv_apply_deferred=0 admit_busy=0 kv_flushes=0 kv_compactions=0',
           '  leaders ' + ' '.join(f'group{g}=node{lead + 1 if lead >= 0 else 0}'
                                    for g, (_, _, lead, _, _, _) in groups.items())]
    for g, (hosted, role, lead, term, voters, retired) in groups.items():
        out.append(f'  group {g} hosted={hosted} role={role if hosted else "-"} leader={lead} '
                   f'term={term} ' + TAIL.format(v=voters, r=retired))
        if role == 'LEADER':
            out.append('    peer 0 next=1 match=3 inflight=0..0 inflight_age=- ack_age=- sent=0 '
                       'again=0 blocked=0 errors=0 rejects=0 reject_hint=0 busy=0 busy_hold=0')
    return '\n'.join(out) + '\n'


# Real output captured on intel2 (3 loopback members, node 1 leads group 0).
REAL = """node 1 root=1 salt=6438199982882807812 groups=2
  bound kv_mt_bytes=998 kv_mem_budget=134217728 kv_l0_bytes=0 kv_l0_cap=1073741824 kv_apply_deferred=0 admit_busy=0 kv_flushes=0 kv_compactions=0
  leaders group0=node1 group2=node2
  group 0 hosted=1 role=LEADER leader=0 term=1 commit=3 applied=3 voters=0x7 joint=0 retired=0 last=3 last_term=1 snap=0 rejects_sent=0 proto_drops=0 unapplied=0 max_unapplied=524288 busy_sent=0 admit_busy=0 low_capacity=0 campaigns_held=0 capacity_stepdowns=0
    peer 0 next=1 match=3 inflight=0..0 inflight_age=- ack_age=- sent=0 again=0 blocked=0 errors=0 rejects=0 reject_hint=0 busy=0 busy_hold=0
    peer 1 next=4 match=3 inflight=0..0 inflight_age=- ack_age=6 sent=3 again=0 blocked=0 errors=0 rejects=0 reject_hint=0 busy=0 busy_hold=0
    peer 2 next=4 match=3 inflight=0..0 inflight_age=- ack_age=6 sent=3 again=0 blocked=0 errors=0 rejects=0 reject_hint=0 busy=0 busy_hold=0
  group 2 hosted=1 role=FOLLOWER leader=1 term=1 commit=3 applied=3 voters=0x7 joint=0 retired=0 last=3 last_term=1 snap=0 rejects_sent=0 proto_drops=0 unapplied=0 max_unapplied=524288 busy_sent=0 admit_busy=0 low_capacity=0 campaigns_held=0 capacity_stepdowns=0
"""

# 1. parsing
s = lp.parse_raft_status(REAL)
assert s['node'] == 1, s
assert s['groups'][0] == {'hosted': 1, 'role': 'leader', 'leader': 1, 'term': 1,
                          'voters': 7, 'retired': 0}, s
assert s['groups'][2]['role'] == 'follower' and s['groups'][2]['leader'] == 2, s
s5 = lp.parse_raft_status(status(5, {0: (0, '-', -1, 0, '0x7', 0),
                                     2: (1, 'CANDIDATE', -1, 9, '0xe', 0)}))
assert s5['node'] == 5 and s5['groups'][0]['hosted'] == 0 and s5['groups'][0]['leader'] == 0, s5
assert s5['groups'][2] == {'hosted': 1, 'role': 'candidate', 'leader': 0, 'term': 9,
                           'voters': 0xe, 'retired': 0}, s5
try:
    lp.parse_raft_status('raft-status rc=-5 (host off?)\n')
    raise AssertionError('error output parsed')
except ValueError:
    pass

# 2. leaders from one sample
n1 = lp.parse_raft_status(status(1, {0: (1, 'LEADER', 0, 4, '0x7', 0),
                                     2: (1, 'LEADER', 0, 3, '0x7', 1)}))   # retired replica
n2 = lp.parse_raft_status(status(2, {0: (1, 'FOLLOWER', 0, 4, '0x7', 0),
                                     2: (1, 'LEADER', 1, 5, '0xe', 0)}))
n3 = lp.parse_raft_status(status(3, {0: (1, 'LEADER', 2, 5, '0x7', 0),   # newer term wins
                                     2: (1, 'FOLLOWER', 1, 5, '0xe', 0)}))
assert lp.leaders_of([n1, n2]) == {0: 1, 2: 2}
assert lp.leaders_of([n1, n2, n3]) == {0: 3, 2: 2}
assert lp.leaders_of([n3, n1, n2]) == {0: 3, 2: 2}
assert lp.leaders_of([n2]) == {0: 0, 2: 2}
assert lp.leaders_of([], groups=[0, 2]) == {0: 0, 2: 0}

# 3. member table
STATUS = """Cluster nodes (3):
  node 1: 127.0.0.1:39561  up  used=0 B  quota=1.00 GiB (0.0%)  storage(1):
      /tmp/x/n1
  node 3: 127.0.0.1:60329  DOWN
  node 2: 127.0.0.1:47881  up  used=0 B  quota=1.00 GiB (0.0%)  storage(1):
Metadata leader placement (self-reported):
  node 1: leads group 0 (term 1)
  node 3: down
  node 2: leads group 2 (term 1)
  leaders: 2/2 groups on 2 distinct node(s); 2/3 members reported
"""
assert lp.parse_members(STATUS) == {1: ('127.0.0.1:39561', True), 2: ('127.0.0.1:47881', True),
                                    3: ('127.0.0.1:60329', False)}
try:
    lp.parse_members(STATUS.replace('(3)', '(4)'))
    raise AssertionError('short member table accepted')
except ValueError:
    pass

# 4. time-weighted summary: 10 s in all
samples = [(0.0, {0: 1, 2: 2}),     # 2 s spread
           (2.0, {0: 1, 2: 1}),     # 3 s together on node 1
           (5.0, {0: 0, 2: 1}),     # 1 s group 0 leaderless
           (6.0, {0: 1, 2: 3}),     # 4 s spread (group 0 back on node 1: no change)
           ]
r = lp.summarize(samples, end=10.0)
assert r['seconds'] == 10.0 and r['samples'] == 4 and r['groups'] == [0, 2], r
assert r['by_group'][0] == {0: 0.1, 1: 0.9}, r
assert r['by_group'][2] == {1: 0.4, 2: 0.2, 3: 0.4}, r
assert abs(r['all_led'] - 0.9) < 1e-12 and abs(r['together'] - 0.3) < 1e-12, r
assert abs(r['together_of_led'] - 0.3 / 0.9) < 1e-12, r
assert r['distinct_nodes'] == {1: 0.3, 2: 0.6}, r
assert r['changes'] == {0: 0, 2: 2}, r
assert lp.summarize([(0.0, {0: 2, 2: 2})], end=4.0)['together'] == 1.0
txt = lp.format_report(r, members=[1, 2, 3], events=[{'kind': 'stepdown', 'rc': 0},
                                                    {'kind': 'stepdown', 'rc': 1}], label='fixture')
assert '| 1 | 90.0 % | 40.0 % |' in txt and '| no leader | 10.0 % | 0.0 % |' in txt, txt
assert 'All leaders on one node: 30.0 % of the run (33.3 %' in txt, txt
assert 'stepdown ok: 1' in txt and 'stepdown refused/failed: 1' in txt, txt

# 5. event order
assert lp.schedule(4, 2) == ['stepdown', 'restart', 'stepdown', 'stepdown', 'restart', 'stepdown']
assert lp.schedule(0, 2) == ['restart', 'restart'] and lp.schedule(3, 0) == ['stepdown'] * 3

print('leader placement: raft-status/status parsing, self-reported leaders by term, '
      'time-weighted shares and event schedule PASS')
