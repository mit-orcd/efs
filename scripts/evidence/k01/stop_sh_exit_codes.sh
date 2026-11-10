#!/bin/bash
# stop_sh_exit_codes.sh STOP_SH
# Run devops nuc-efs/stop.sh --daemons-only against a stub server_processes.py
# that exits with scripted codes for nodes n1..n3 and print, per scenario, the
# script's exit code, which nodes were asked to stop, and its non-stop-file
# output. Needs only bash and python3 (runs on macOS); no daemon, host or
# cluster is touched. Used by docs/status/2026-11/k01b-stop-sh-patch-20261110.md.
#   git -C ~/git/devops show devel:nuc-efs/stop.sh > /tmp/stop.old.sh
#   git -C ~/git/devops show k01b-stop-nonorderly:nuc-efs/stop.sh > /tmp/stop.new.sh
#   scripts/evidence/k01/stop_sh_exit_codes.sh /tmp/stop.old.sh
#   scripts/evidence/k01/stop_sh_exit_codes.sh /tmp/stop.new.sh
set -u
STOP=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
scenario() { # rc1 rc2 rc3
  local T; T=$(mktemp -d); mkdir -p "$T/src/scripts" "$T/logs"
  cat > "$T/src/scripts/server_processes.py" <<PY
import sys,re,os
n=re.search(r'efsd-n(\d+)\.pid',sys.argv[2]).group(1)
open("$T/calls","a").write("n"+n+"\n")
code=os.environ["FAKE_RCS"].split(",")[int(n)-1]
print("fake stop n"+n+" rc "+code,flush=True)
sys.exit(int(code))
PY
  cp "$STOP" "$T/stop.sh"
  cat > "$T/env.sh" <<ENV
BASE=$T
LOGS=$T/logs
EFS_CLUSTER=intel
LOCAL_HOST=fixture
PORTS=(1 2 3)
NODE_HOSTS=(fixture fixture fixture)
STORDIRS=($T/n1 $T/n2 $T/n3)
MOUNTS=($T/mnt)
ENV
  FAKE_RCS=$1,$2,$3 /bin/bash "$T/stop.sh" --daemons-only >"$T/out" 2>"$T/err"; local rc=$?
  echo "stop-file rcs=$1,$2,$3 -> exit=$rc nodes_stopped=$(tr '\n' ' ' < "$T/calls")"
  grep -v '^fake stop' "$T/out" | sed 's/^/   stdout: /'; sed 's/^/   stderr: /' "$T/err"
  rm -rf "$T"
}
for s in 0,0,0 0,4,0 4,0,0 3,0,4 4,0,3 4,3,4 1,0,4 0,0,3; do IFS=, read -r a b c <<< "$s"; scenario "$a" "$b" "$c"; done
