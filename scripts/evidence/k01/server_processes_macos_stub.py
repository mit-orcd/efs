#!/usr/bin/env python3
"""macOS stand-in for scripts/server_processes.py, for stop_sh_fixture_macos.sh only.

record: no-op. stop-file: owns a PID only when its executable is named efsd and its
last argument is the storage root; sends SIGTERM, waits for exit, then reports
orderly (rc 0) when <storage>/log/efsd.exit names that PID with outcome=orderly,
else outcome unknown (rc 4). It does not check the barrier lines, the stale
identity stamp or the forced/barrier-failed records, and uses ps instead of /proc
and pidfd. It checks the fixture's logic, not the real helper.
"""
import os,signal,subprocess,sys,time
from pathlib import Path
cmd,file,storage=sys.argv[1],Path(sys.argv[2]),sys.argv[3]
if cmd=='record':sys.exit(0)
def ps(pid):
    r=subprocess.run(['ps','-o','stat=,command=','-p',str(pid)],capture_output=True,text=True).stdout.strip()
    return r
if not file.exists():sys.exit(0)
pid=int(file.read_text().strip()); out=ps(pid)
args=out.split(None,1)[1].split() if out else []
if not args or Path(args[0]).name!='efsd' or os.path.realpath(args[-1])!=os.path.realpath(storage):
    print(f'Stale daemon identity for PID {pid}; process retained');file.unlink();sys.exit(0)
rec=Path(storage)/'log/efsd.exit'; rec.unlink(missing_ok=True)
os.kill(pid,signal.SIGTERM)
for _ in range(100):
    o=ps(pid)
    if not o or o.startswith('Z'):break
    time.sleep(.05)
file.unlink(missing_ok=True)
ok=rec.exists() and f'pid={pid}\n' in rec.read_text() and 'outcome=orderly' in rec.read_text()
print(f'efsd PID {pid} stopped, '+('orderly' if ok else 'outcome unknown; not an orderly stop'))
sys.exit(0 if ok else 4)
