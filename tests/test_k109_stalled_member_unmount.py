#!/usr/bin/env python3
"""K109: a clean unmount completes while one storage member is alive but stalled.

Three private fault-build efsd; node 3 runs with its compactor parked and a small
L0 budget, so its apply defers and it answers the storage fence with an error
(NOT_PRIMARY) instead of sealing. Three production unmounts (scripts/client.sh
stop) must exit 0 with rc=0 in under 5 s: nodes 1 and 2 are a strict majority and
the fence does not depend on node 3 (it failed after 15 s with rc=-15
phase=storage-fence before the fix). Then node 2 stalls too: one member of three
sealed, so the next unmount must fail closed at phase=storage-fence (15 s).
About 90 s; make test-long. Usage: test_k109_stalled_member_unmount.py [TREE]
"""
import subprocess, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
r = subprocess.run([sys.executable, str(root / 'tests/live/k109_stalled_member_unmount.py'), '--runs', '3',
                    '--refused', '1'], cwd=root, capture_output=True, text=True, timeout=900)
print(r.stdout[-3000:], r.stderr[-1500:])
assert r.returncode == 0 and 'PASS 4/4' in r.stdout, 'unmount with a stalled member did not behave as specified'
print('K109 unmount with one stalled member: exit 0, rc=0, fast; with two stalled: fails closed PASS')
