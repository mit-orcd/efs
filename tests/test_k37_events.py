#!/usr/bin/env python3
"""K37 regression: growth in an earlier node log must not select old losses."""
from k37_events import degraded_write_events

def line(sid):
    return f'packer: segment g2 id={sid:016x} gen=1 written degraded: fragment 2 for node 3\n'

old_node1 = line(1)
old_node2 = line(2)
snapshot = degraded_write_events(old_node1 + old_node2)
# A leader on node 1 emits a new segment while node 2's old log stays.
text = old_node1 + line(3) + old_node2
assert degraded_write_events(text)[len(snapshot):] == [(2, 2, 1, 2, 3)], 'counterexample must expose the old count cursor'
assert degraded_write_events(text, snapshot) == [(2, 3, 1, 2, 3)]
# Both logs can grow; duplicate notices must not count as extra segments.
assert degraded_write_events(text + line(4) + line(3), snapshot) == [(2, 3, 1, 2, 3), (2, 4, 1, 2, 3)]
assert degraded_write_events(text, degraded_write_events(text)) == []
print('PASS K37 event identities: reordered log growth, duplicate notices, no old fault targets')
