#!/usr/bin/env python3
"""K54: a clean unmount completes with member 2 down.

Member 2 hosts both metadata groups, so with it down the appends cleanup step
(a request only a member hosting both groups can run) was sent by the client's
first voter to member 2 again and again: the unmount failed after 20 s with
phase=cleanup. Two production unmounts (efs-fuse, scripts/client.sh stop) on four
real efsd, member 2 SIGKILLed and then stopped cleanly, must exit 0 with rc=0 and
every file reads back exact once member 2 returns. About 25 s; make test-long.
Usage: test_k54_member2_unmount.py [TREE]
"""
import subprocess, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
r = subprocess.run([sys.executable, str(root / 'tests/live/k54_member_down_unmount.py'), '--runs', '2',
                    '--mode', 'alternate', '--victim', '2'], cwd=root, capture_output=True, text=True, timeout=600)
print(r.stdout[-3000:], r.stderr[-1500:])
assert r.returncode == 0 and 'PASS 2/2' in r.stdout, 'unmount with member 2 down did not complete cleanly'
print('K54 unmount with member 2 down (SIGKILL, then clean stop): exit 0, revoke rc=0, files exact PASS')
