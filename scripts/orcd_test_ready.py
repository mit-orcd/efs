#!/usr/bin/env python3
"""Read-only readiness barrier for an existing ORCD-test filesystem.
Membership, a self-reported leader for each hosted group, applied recovery
floors and a fresh root read are required. Never formats or repairs a store.
"""
import argparse
import concurrent.futures
import re
import subprocess
import time


def inspect(binary, address):
    result = subprocess.run([binary, 'raft-status', address], capture_output=True,
                            text=True, timeout=10)
    if result.returncode:
        raise RuntimeError((address, result.returncode, result.stderr))
    header = re.search(r'node (\d+) root=(\d+) salt=(\d+) groups=(\d+)', result.stdout)
    if not header:
        raise RuntimeError((address, result.stdout))
    groups = {}
    for match in re.finditer(r'group (\d+) hosted=(\d+) role=(\S+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+) voters=0x([0-9a-f]+) joint=(\d+)', result.stdout):
        group, hosted, role, leader, term, commit, applied, voters, joint = match.groups()
        line = result.stdout[match.start():].split('\n', 1)[0]
        retired = bool(re.search(r'\bretired=1\b', line))
        groups[int(group)] = dict(hosted=int(hosted), retired=retired, role=role, leader=int(leader),
            term=int(term), commit=int(commit), applied=int(applied), voters=int(voters, 16), joint=int(joint))
    return dict(node=int(header[1]), root=int(header[2]), salt=int(header[3]), groups=groups,
                address=address, raw=result.stdout)


def ready(states):
    assert len({s['node'] for s in states}) == len(states), 'duplicate nodes'
    assert len({s['salt'] for s in states}) == 1 and states[0]['salt'], 'export not recovered'
    groups = set().union(*(s['groups'] for s in states))
    assert groups, 'no groups recovered'
    assert all(set(s['groups']) == groups for s in states), 'incomplete group view'
    leaders = {}
    for group in groups:
        # A retained retired replica is not a current voter. The equality below
        # still refuses every missing member of the committed voter mask.
        hosted = [s for s in states if s['groups'][group]['hosted'] and
                  not s['groups'][group].get('retired', False)]
        own = [s for s in hosted if s['groups'][group]['role'] == 'LEADER']
        assert len(own) == 1, ('leader unavailable', group)
        leader = own[0]; g = leader['groups'][group]
        assert {s['node']-1 for s in hosted} == {i for i in range(32) if g['voters'] & (1 << i)}, ('missing voter', group)
        assert g['leader'] == leader['node']-1 and not g['joint'], ('unstable leader', group)
        assert g['commit'] > 0 and g['applied'] >= g['commit'], ('leader recovering', group)
        assert all(s['groups'][group]['leader'] == g['leader'] and
                   s['groups'][group]['term'] == g['term'] and
                   s['groups'][group]['voters'] == g['voters'] and
                   not s['groups'][group]['joint'] for s in hosted), ('views disagree', group)
        leaders[group] = leader
    assert 0 in leaders and leaders[0]['root'], 'root not recovered'
    return leaders


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mgmt', default='./efs-mgmt')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('addresses', nargs='+')
    args = parser.parse_args()
    deadline = time.monotonic()+args.timeout
    previous = None
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(args.addresses)) as pool:
        while time.monotonic() < deadline:
            try:
                states = list(pool.map(lambda address: inspect(args.mgmt, address), args.addresses))
                leaders = ready(states)
                identity = tuple((g, s['node'], s['groups'][g]['term']) for g, s in sorted(leaders.items()))
                result = subprocess.run([args.mgmt, 'raft-getattr', leaders[0]['address'], '1'],
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode == 0 and re.search(r'raft-getattr status=0\b', result.stdout), result.stdout
                if previous == identity:
                    print('READINESS PASS: stable leaders, applied root and fresh read', flush=True)
                    for state in states: print(state['raw'], end='')
                    return
                previous = identity
            except (AssertionError, RuntimeError, subprocess.TimeoutExpired) as error:
                previous = None
                print('not ready: '+str(error), flush=True)
            time.sleep(1)
    raise SystemExit('READINESS FAIL: deadline exceeded; retain all stores and logs')


if __name__ == '__main__':
    main()
