"""Bounded topology setup for the forwarded-publication regression.

A formed membership does not imply that a leadership-transfer target has
matched the leader's tail. BUSY is a valid setup response, not a failed
publication. No workload operation or correctness assertion is retried.
"""
import re
import time


def split_leaders(cluster, mgmt, timeout_s, clock=time.monotonic, sleep=time.sleep):
    deadline = clock() + timeout_s
    attempts = []
    place = cluster.leader_placement(1)
    while place[0] == place[2] and clock() < deadline:
        by_id = {cluster.node_id(i): i for i in (1, 2, 3)}
        target = next(n for n in sorted(by_id) if n != place[0])
        rc, out = mgmt(cluster.env, 'raft-step-down', cluster.ep(by_id[place[2]]),
                       '2', str(target))
        attempts.append(out.strip())
        if rc:
            status = re.search(r'\brc=(-?\d+)\b', out)
            # BUSY: catch-up/configuration/persistence not ready. NOT_PRIMARY:
            # leadership changed since the read; observe the new placement.
            assert status and int(status[1]) in (-13, -15), out
        sleep(min(0.5, max(0, deadline - clock())))
        place = cluster.leader_placement(1)
    assert place[0] != place[2], f'leaders did not separate in {timeout_s}s: {place}; requests={attempts}'
    return place
