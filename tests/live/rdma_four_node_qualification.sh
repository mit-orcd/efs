#!/usr/bin/env bash
# Exclusive AMD, private files/direct4G fixture. No installed stores touched.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=${EFS_RDMA_DEV:-rxe0} EFS_RDMA_GID_INDEX=${EFS_RDMA_GID_INDEX:-1}
export EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20
export EFS_MD_RAFT_N=4 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0
unset EFS_READ_PROF EFS_RPC_PROF EFS_CONTAINER_PATH_PROF EFS_READ_FRAG_PROF
base=${EFS_GATE_BASE_PORT:-27432}
work=$(mktemp -d /data1/efs/rdma-qualification-XXXXXX)
home=$(mktemp -d /home/efs/rdma-qualification-XXXXXX)
mkdir -p "$home/mnt" "$home/mnt2"
printf '%s\n' "$home" > "$work/mount-root.txt"
printf '%s\n' "$work" # raw artifacts stay outside Git
pids=();rc=0
stop_nodes() {
 for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
 for pid in "${pids[@]}"; do wait "$pid" || true; done
 pids=()
}
stop_mounts() {
 for mount in "$home/mnt" "$home/mnt2"; do
  if mountpoint -q "$mount"; then "$src/scripts/client.sh" stop "$mount" || return 1; fi
 done
}
cleanup() {
 if stop_mounts; then stop_nodes; else echo 'Private fixture retained: clean mount drain failed' >&2; fi
}
trap cleanup EXIT
start_nodes() {
 for id in 1 2 3 4; do
  join=();[ "$id" = 1 ] || join=(--join "127.0.0.1:$base")
  "$src/efsd" --node-id "$id" --addr 127.0.0.1 --port "$((base+id-1))" --storage "$work/n$id" --storage-engine files --quota 4G --direct-io "${join[@]}" >> "$work/n$id.log" 2>&1 &
  pids+=("$!");printf '%s\n' "$!" > "$work/efsd-n$id.pid"
  ready=0
  for attempt in $(seq 1 60); do
   kill -0 "${pids[-1]}" || break
   if "$src/efs-mgmt" raft-status "127.0.0.1:$((base+id-1))" > "$work/n$id-status.txt" 2>/dev/null; then ready=1;break;fi
   sleep .5
  done
  [ "$ready" = 1 ] || { tail -n 15 "$work/n$id.log";return 1; }
 done
}
mount_clients() {
 for name in mnt mnt2; do
  EFS_FUSE_LOG="$work/fuse-$name.log" "$src/scripts/client.sh" "127.0.0.1:$base" "$home/$name"
 done
}
start_nodes
formatted=0
for attempt in $(seq 1 60); do
 reply=$("$src/efs-mgmt" raft-mkfs "127.0.0.1:$base") || true
 if [[ "$reply" == *'rc=0 '* ]]; then formatted=1;break;fi
 sleep .5
done
[ "$formatted" = 1 ] || { echo "$reply";exit 1; }
mount_clients
for round in 1; do
 python3 -u "$src/tests/live/rdma_installed_load.py" "$src" "$work/load$round" --base-port "$base" > "$work/load$round.log" 2>&1 || rc=1
 tail -n 3 "$work/load$round.log"
 # Stop on dirty fixtures; never layer another run on unresolved clients.
 python3 - "$work/load$round/cleanup.json" <<'PY' || exit 1
import json,sys
assert not json.load(open(sys.argv[1]))['errors']
PY
done
python3 "$src/tests/posix/posix_2client.py" --local "$home/mnt" "$home/mnt2" --parent rdma-fixed-peer --results "$work/peer.tsv" > "$work/peer.log" 2>&1 || rc=1
python3 "$src/tests/posix/posix_persist.py" "$home/mnt" --phase prepare --tag rdma-fixed --results "$work/persist-prepare.tsv" > "$work/persist-prepare.log" 2>&1 || rc=1
stop_mounts
stop_nodes
start_nodes
mount_clients
python3 "$src/tests/posix/posix_persist.py" "$home/mnt" --phase verify --tag rdma-fixed --results "$work/persist-verify.tsv" > "$work/persist-verify.log" 2>&1 || rc=1
if [ "${EFS_GATE_DEGRADED:-1}" = 1 ]; then
python3 -u "$src/tests/live/rdma_degraded_read.py" "$src" "$home/mnt" "$work/degraded" --base-port "$base" --pid-root "$work" > "$work/degraded.log" 2>&1 || rc=1
fi
for log in peer persist-prepare persist-verify degraded; do [ ! -f "$work/$log.log" ] || tail -n 3 "$work/$log.log";done
printf '%s\n' "$rc" > "$work/exit-code"
# Qualification stores are synthetic and disposable after clean teardown.
# Retain all verdicts/logs; never carry old payload capacity into a new cohort.
stop_mounts
stop_nodes
for id in 1 2 3 4; do
 [ ! -L "$work/n$id" ] || exit 1
 rm -rf -- "$work/n$id"
done
exit "$rc"
