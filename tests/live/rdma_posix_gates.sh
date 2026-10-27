#!/usr/bin/env bash
# Private three-member POSIX-all and peer gate; only owned stores/processes.
# Usage: EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 EFS_GATE_ROOT=/tmp/owned bash "$0" [19xxx-base-port]
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
mode=${EFS_TRANSPORT:-rdma}
case "$mode" in tcp|rdma) ;; *) echo 'Use explicit tcp or rdma' >&2; exit 2;; esac
export EFS_TRANSPORT=$mode
export EFS_RDMA_DEV=${EFS_RDMA_DEV:-rxe0}
if [ "$mode" = rdma ]; then
    test -d "/sys/class/infiniband/$EFS_RDMA_DEV" || { echo 'RDMA device missing; gates NOTRUN' >&2; exit 3; }
fi
port=${1:-19390}
python3 - "$port" <<'PY'
import socket,sys
base=int(sys.argv[1]);assert 19000 <= base <= 19997, 'use a private 19xxx range'
held=[]
for p in range(base,base+3):
    s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    s.bind(('127.0.0.1',p));held.append(s)
PY
run=$(mktemp -d "${EFS_GATE_ROOT:-${TMPDIR:-/tmp}}/gate-$mode-XXXXXX")
chmod 755 "$run"
printf 'Evidence: %s\n' "$run"
mkdir "$run/mnt" "$run/mnt2"; pids=()
cleanup() {
    rc=$?; trap - EXIT
    # A failed clean stop retains the private daemons and stores for diagnosis.
    # Never lazy-detach or kill the client to manufacture successful cleanup.
    for m in "$run/mnt" "$run/mnt2"; do
        if ! "$src/scripts/client.sh" stop "$m" >> "$run/cleanup.log" 2>&1; then
            echo "Retained private fixture: clean client stop failed at $m" >&2
            exit 1
        fi
    done
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    end=$((SECONDS+60))
    for pid in "${pids[@]}"; do
        while kill -0 "$pid" 2>/dev/null; do
            if (( SECONDS >= end )); then
                echo "Private daemon $pid did not stop within 60 s; fixture retained" >&2
                exit 1
            fi
            sleep .1
        done
        if ! wait "$pid"; then rc=1; fi
    done
    printf '%s\n' "$rc" > "$run/exit-status"
    exit "$rc"
}
trap cleanup EXIT
for id in 1 2 3; do
    ep=127.0.0.1:$((port+id-1)); join=()
    [ "$id" = 1 ] || join=(--join "127.0.0.1:$port" --listen "$ep")
    if [ "$id" = 1 ]; then join=("$ep"); fi
    "$src/efsd" "${join[@]}" "$run/n$id" --quota 4G --discovery-port 0 > "$run/n$id.log" 2>&1 &
    pids+=("$!")
    ready=0
    for attempt in $(seq 1 60); do
        if grep -q 'listening on' "$run/n$id.log"; then ready=1; break; fi
        kill -0 "${pids[-1]}" 2>/dev/null || break; sleep .5
    done
    [ "$ready" = 1 ] || { cat "$run/n$id.log"; exit 1; }
done
python3 - "$src" "127.0.0.1:$port" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,str(Path(sys.argv[1])/'tests/live'))
from raft_form import form
form(Path(sys.argv[1])/'efs-mgmt',sys.argv[2])
PY
for m in "$run/mnt" "$run/mnt2"; do "$src/scripts/client.sh" "127.0.0.1:$port" "$m"; done
python3 - "$src" "$run" "$mode" "${pids[@]}" <<'PY'
from pathlib import Path
import json,os,sys,time
src,run=map(Path,sys.argv[1:3]);pids=list(map(int,sys.argv[4:]));mounts={str(run/'mnt'),str(run/'mnt2')}
for proc in Path('/proc').iterdir():
    if not proc.name.isdigit():continue
    try:
        args=[a.decode() for a in (proc/'cmdline').read_bytes().split(b'\0') if a]
        if args and Path(args[0]).resolve()==src/'efs-fuse' and mounts.intersection(args):
            assert proc.stat().st_uid==os.getuid();pids.append(int(proc.name))
    except (FileNotFoundError,PermissionError,ProcessLookupError):continue
assert len(set(pids))==5,pids
proof=[]
for pid in pids:
    fds=Path('/proc')/str(pid)/'fd';devices=[];end=time.monotonic()+20
    while True:
        for fd in fds.iterdir():
            try:target=os.readlink(fd)
            except FileNotFoundError:continue
            if target.startswith('/dev/infiniband/uverbs'):devices.append(target)
        if devices or sys.argv[3]!='rdma' or time.monotonic()>=end:break
        time.sleep(.1)
    assert devices or sys.argv[3]!='rdma',('no verbs device',pid)
    proof.append(dict(pid=pid,verbs_devices=sorted(set(devices))))
(run/'transport-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
PY
rc=0
python3 "$src/tests/posix/posix_suite.py" "$run/mnt" --suite all --jobs 1 --results "$run/posix.tsv" > "$run/posix.log" 2>&1 || rc=1
python3 "$src/tests/posix/posix_2client.py" --local "$run/mnt" "$run/mnt2" --parent rdma-peer-gate --results "$run/posix2.tsv" > "$run/posix2.log" 2>&1 || rc=1
"$src/efs-mgmt" status "127.0.0.1:$port" > "$run/status.txt" || rc=1
tail -5 "$run/posix.log"; tail -5 "$run/posix2.log"
exit "$rc"
