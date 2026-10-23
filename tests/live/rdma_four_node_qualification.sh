#!/usr/bin/env bash
# Exclusive AMD, private files/direct4G fixture. No installed stores touched.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=${EFS_RDMA_DEV:-rxe0} EFS_RDMA_GID_INDEX=${EFS_RDMA_GID_INDEX:-1}
export EFS_GC_BACKLOG_IDLE_MS=20
# Explicit placement qualification.
export EFS_METADATA_BALANCE=1
export EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0
unset EFS_READ_PROF EFS_RPC_PROF EFS_DEEP_PATH_PROF EFS_READ_FRAG_PROF
base=${EFS_GATE_BASE_PORT:-27432}
work=$(mktemp -d /data1/efs/rdma-qualification-XXXXXX)
client_root=$(mktemp -d /home/efs/rdma-qualification-XXXXXX)
mkdir -p "$client_root/mnt" "$client_root/mnt2"
printf '%s\n' "$client_root" > "$work/mount-root.txt"
printf '%s\n' "$work" # raw artifacts stay outside Git
# D39: the configured member list; efs-mgmt declares as management with it.
for id in 1 2 3 4; do echo "$id 127.0.0.1:$((base+id-1))"; done > "$work/cluster.conf"
export EFS_CLUSTER_CONF="$work/cluster.conf"
pids=();rc=0
stop_nodes() {
 for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
 for pid in "${pids[@]}"; do wait "$pid" || true; done
 pids=()
}
stop_mounts() {
 for mount in "$client_root/mnt" "$client_root/mnt2" "$client_root/mnt-eof"; do
  if mountpoint -q "$mount"; then "$src/scripts/client.sh" stop "$mount" || return 1; fi
 done
}
cleanup() {
 if stop_mounts; then stop_nodes; else echo 'Private fixture retained: clean mount drain failed' >&2; fi
}
trap cleanup EXIT
start_nodes() {
 for id in 1 2 3 4; do
  "$src/efsd" --cluster "$work/cluster.conf" --addr 127.0.0.1 --port "$((base+id-1))" --storage "$work/n$id" --quota 4G --direct-io >> "$work/n$id.log" 2>&1 &
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
  EFS_FUSE_LOG="$work/fuse-$name.log" "$src/scripts/client.sh" "127.0.0.1:$base" "$client_root/$name"
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
# single runs ordinary serial and one-mount parallel POSIX, never seven clients.
case "${EFS_GATE_LOAD_MODE:-seven}" in
 single)
  for jobs in 1 16; do
   python3 -u "$src/tests/posix/posix_suite.py" "$client_root/mnt" --suite core --jobs "$jobs" --tag "rdma-single-$jobs" --results "$work/core-$jobs.tsv" > "$work/core-$jobs.log" 2>&1 || rc=1
   tail -n 3 "$work/core-$jobs.log"
   # A live timed-out worker makes further mutation unsafe.
   if grep -q 'worker.*still alive' "$work/core-$jobs.log"; then exit 1;fi
  done
  python3 "$src/tests/posix/posix_extras.py" "$client_root/mnt" --results "$work/extras.tsv" > "$work/extras.log" 2>&1 || rc=1
  ;;
 seven)
for round in 1; do
 python3 -u "$src/tests/live/rdma_installed_load.py" "$src" "$work/load$round" --base-port "$base" > "$work/load$round.log" 2>&1 || rc=1
 tail -n 3 "$work/load$round.log"
 # Stop on dirty fixtures; never layer another run on unresolved clients.
 python3 - "$work/load$round/cleanup.json" <<'PY' || exit 1
import json,sys
assert not json.load(open(sys.argv[1]))['errors']
PY
done
  ;;
 *) echo 'EFS_GATE_LOAD_MODE must be single or seven' >&2;exit 2;;
esac
python3 "$src/tests/posix/posix_2client.py" --local "$client_root/mnt" "$client_root/mnt2" --parent rdma-fixed-peer --results "$work/peer.tsv" > "$work/peer.log" 2>&1 || rc=1
# Optional W100 faulted client uses the same source/build ID as native daemons.
# Only this private mount enables omission; installed clients remain native.
if [ -n "${EFS_GATE_EOF_SOURCE:-}" ]; then
 [ "$(cat "$EFS_GATE_EOF_SOURCE/SOURCE_COMMIT")" = "$(cat "$src/SOURCE_COMMIT")" ] || exit 1
 printf 'OFF\n' > "$work/omit-size"
 printf 'OFF\n' > "$work/other-faults"
 mkdir -p "$client_root/mnt-eof"
 EFS_FUSE_LOG="$work/fuse-eof.log" EFS_FAULT_REPORT_SIZE_FILE="$work/omit-size" EFS_FAULT_REPORT_FILE="$work/other-faults" "$EFS_GATE_EOF_SOURCE/scripts/client.sh" "127.0.0.1:$base" "$client_root/mnt-eof"
 timeout -k 5 180s python3 "$src/tests/live/report_eof_contract.py" "$client_root/mnt-eof" "$work/omit-size" --expect refused --peer-mount "$client_root/mnt2" > "$work/eof.log" 2>&1 || rc=1
 printf 'OFF\n' > "$work/omit-size"
 "$src/scripts/client.sh" stop "$client_root/mnt-eof" || exit 1
 tail -n 2 "$work/eof.log"
fi
python3 "$src/tests/posix/posix_persist.py" "$client_root/mnt" --phase prepare --tag rdma-fixed --results "$work/persist-prepare.tsv" > "$work/persist-prepare.log" 2>&1 || rc=1
stop_mounts
stop_nodes
start_nodes
mount_clients
python3 "$src/tests/posix/posix_persist.py" "$client_root/mnt" --phase verify --tag rdma-fixed --results "$work/persist-verify.tsv" > "$work/persist-verify.log" 2>&1 || rc=1
if [ "${EFS_GATE_DEGRADED:-1}" = 1 ]; then
python3 -u "$src/tests/live/rdma_degraded_read.py" "$src" "$client_root/mnt" "$work/degraded" --base-port "$base" --pid-root "$work" > "$work/degraded.log" 2>&1 || rc=1
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
