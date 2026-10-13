#!/usr/bin/env python3
"""Installed AMD: natural 512MiB snapshots, lag one shared follower, restore.

No threshold, quota, timeout or transport-window changes. Run only with exclusive
access to the four-node installed AMD fixture; retains raw output outside Git.
Both group workloads use the existing 600-second pressure script. Follower is
selected only after its role is FOLLOWER in both groups; restored on every exit.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
p.add_argument('mount', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
mgmt = a.source / 'efs-mgmt'
statuses = {}
for node in (2, 3):
    s = subprocess.check_output([str(mgmt), 'raft-status', f'127.0.0.1:{17431+node}'], text=True, timeout=15)
    statuses[node] = s
candidate = next((n for n, s in statuses.items() if s.count('role=FOLLOWER') == 2), None)
assert candidate, statuses
pid = int(Path(f'/data1/efs/logs/efsd-n{candidate}.pid').read_text())
cmd = Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode()
assert f'--node-id {candidate} ' in cmd and '--storage-engine files' in cmd
(a.output/'before.json').write_text(json.dumps(dict(node=candidate, pid=pid, statuses=statuses), indent=2))
children = []
streams = []
stopped = False

def abort(signum, _frame):
    raise KeyboardInterrupt(f'signal {signum}')
signal.signal(signal.SIGTERM, abort)
signal.signal(signal.SIGINT, abort)
try:
    for group in (0, 2):
        log = (a.output/f'group{group}.log').open('w')
        streams.append(log)
        children.append(subprocess.Popen(['python3', str(a.source/'tests/live/efs_snapshot_pressure.py'), str(a.mount), str(a.output/f'group{group}'), '--group', str(group)], stdout=log, stderr=subprocess.STDOUT))
    # Files and initial data fsync must complete before pausing a data holder.
    end = time.monotonic()+30
    while not all((a.output/f'group{g}/latency.jsonl').exists() for g in (0, 2)):
        assert all(c.poll() is None for c in children), 'pressure failed before lag injection'
        assert time.monotonic()<end, 'pressure setup exceeded original30s budget'
        time.sleep(.1)
    os.kill(pid, signal.SIGSTOP)
    stopped = True
    print(f'Lagging follower node={candidate} pid={pid}; both natural pressure workloads active', flush=True)
    codes = [c.wait(timeout=630) for c in children]  #600s workload + existing30s I/O allowance
    assert not any(codes), codes
finally:
    if stopped:
        os.kill(pid, signal.SIGCONT)
        print(f'Restored follower node={candidate}', flush=True)
    for c in children:
        if c.poll() is None:
            c.terminate()
            try:
                c.wait(timeout=30)
            except subprocess.TimeoutExpired:
                c.kill()
                c.wait(timeout=5)
    for stream in streams:
        stream.close()
# Catch-up uses the existing180s recovery gate budget, never adjusted on failure.
end = time.monotonic()+180
last = ''
while True:
    last = subprocess.check_output([str(mgmt), 'raft-status', f'127.0.0.1:{17431+candidate}'], text=True, timeout=15)
    # Compare durable/applied indices with each group's current leader.
    rows = []
    for line in last.splitlines():
        if 'hosted=1' not in line:
            continue
        fields = dict(x.split('=', 1) for x in line.split() if '=' in x)
        leader = int(fields['leader'])
        if leader < 0:
            rows.append(False)
            continue
        leader_text = subprocess.check_output([str(mgmt), 'raft-status', f'127.0.0.1:{17432+leader}'], text=True, timeout=15)
        group = line.strip().split()[1]
        remote = next(x for x in leader_text.splitlines() if x.strip().startswith(f'group {group} '))
        rf = dict(x.split('=', 1) for x in remote.split() if '=' in x)
        rows.append(int(fields['applied']) >= int(rf['commit']))
    if len(rows) == 2 and all(rows):
        break
    assert time.monotonic()<end, last
    time.sleep(.5)
(a.output/'after.txt').write_text(last)
print('PASS: both natural snapshot workloads and follower applied catch-up; inspect raft-snap import logs for actual transfer', flush=True)
