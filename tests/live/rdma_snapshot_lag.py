#!/usr/bin/env python3
"""Installed AMD: natural 512MiB snapshots, lag one shared follower, restore.

No threshold, quota, timeout or transport-window changes. Run only with exclusive
access to the four-node installed AMD fixture; retains raw output outside Git.
Each independent writer uses the existing 600-second pressure script. Follower is
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
p.add_argument('--writers-per-group', type=int, default=1)
a = p.parse_args()
assert 1 <= a.writers_per_group <= 16
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
assert f'--node-id {candidate} ' in cmd
(a.output/'before.json').write_text(json.dumps(dict(node=candidate, pid=pid, statuses=statuses), indent=2))
children = []
streams = []
stopped = False

def abort(signum, _frame):
    raise KeyboardInterrupt(f'signal {signum}')
signal.signal(signal.SIGTERM, abort)
signal.signal(signal.SIGINT, abort)
try:
    finish = time.monotonic()+630 #600s workload + existing30s I/O allowance
    outputs = []
    for group in (0, 2):
        for writer in range(a.writers_per_group):
            output = a.output/f'group{group}-writer{writer}'
            outputs.append(output)
            log = output.with_suffix('.log').open('w')
            streams.append(log)
            children.append(subprocess.Popen(['python3', str(a.source/'tests/live/efs_snapshot_pressure.py'), str(a.mount), str(output), '--group', str(group)], stdout=log, stderr=subprocess.STDOUT))
    (a.output/'children.json').write_text(json.dumps([c.pid for c in children]))
    # Files and initial data fsync must complete before pausing a data holder.
    end = time.monotonic()+30
    while not all((output/'latency.jsonl').exists() for output in outputs):
        assert all(c.poll() is None for c in children), 'pressure failed before lag injection'
        assert time.monotonic()<end, 'pressure setup exceeded original30s budget'
        time.sleep(.1)
    current = subprocess.check_output([str(mgmt), 'raft-status', f'127.0.0.1:{17431+candidate}'], text=True, timeout=15)
    assert current.count('role=FOLLOWER') == 2, current
    os.kill(pid, signal.SIGSTOP)
    stopped = True
    print(f'Lagging follower node={candidate} pid={pid}; {len(children)} independent natural pressure writers active', flush=True)
    codes = [c.wait(timeout=max(0, finish-time.monotonic())) for c in children]
    assert not any(codes), codes
finally:
    if stopped:
        os.kill(pid, signal.SIGCONT)
        print(f'Restored follower node={candidate}', flush=True)
    # Signal every owned worker before waiting: one blocked FUSE operation must
    # not prevent termination of the remaining workers or mask the gate error.
    for c in children:
        if c.poll() is None:
            c.terminate()
    cleanup_end = time.monotonic()+30
    for c in children:
        if c.poll() is None:
            try:
                c.wait(timeout=max(0, cleanup_end-time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
    for c in children:
        if c.poll() is None:
            c.kill()
    cleanup_end = time.monotonic()+5
    for c in children:
        if c.poll() is None:
            try:
                c.wait(timeout=max(0, cleanup_end-time.monotonic()))
            except subprocess.TimeoutExpired:
                print(f'Cleanup incomplete: owned worker pid={c.pid}', flush=True)
    for stream in streams:
        stream.close()
# Catch-up uses the existing180s recovery gate budget, never adjusted on failure.
end = time.monotonic()+180
def status(port):
    return subprocess.check_output([str(mgmt), 'raft-status', f'127.0.0.1:{port}'], text=True, timeout=min(15, max(.001, end-time.monotonic())))
last = status(17431+candidate)
targets = {}
for line in last.splitlines():
    if 'hosted=1' not in line:
        continue
    fields = dict(x.split('=', 1) for x in line.split() if '=' in x)
    leader = int(fields['leader'])
    assert leader >= 0, last
    group = line.strip().split()[1]
    remote = next(x for x in status(17432+leader).splitlines() if x.strip().startswith(f'group {group} '))
    rf = dict(x.split('=', 1) for x in remote.split() if '=' in x)
    targets[group] = int(rf['commit'])
assert len(targets)==2
# Fixed acknowledged watermark avoids chasing unrelated background GC commits.
while True:
    last = status(17431+candidate)
    applied = {}
    for line in last.splitlines():
        if 'hosted=1' in line:
            fields = dict(x.split('=', 1) for x in line.split() if '=' in x)
            applied[line.strip().split()[1]] = int(fields['applied'])
    if all(applied.get(g, -1)>=goal for g, goal in targets.items()):
        break
    assert time.monotonic()<end, last
    time.sleep(.5)
(a.output/'after.txt').write_text(last)
(a.output/'targets.json').write_text(json.dumps(targets,indent=2))
print('PASS: both natural snapshot workloads and follower applied catch-up; inspect raft-snap import logs for actual transfer', flush=True)
