#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/w73-w64-rxe.lock
flock -n 9 || exit 75
base=/data1/efs
build=$base/gate-rxe-f6233987
out=$base/w73-w64-rxe-f6233987
mkdir -p "$out"
[ "$(cat "$build/.build_id.stamp")" = f6233987b9d1 ]
export EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
"$build/tests/test_rdma_xprt" > "$out/rdma-preflight.log" 2>&1
! grep -q SKIP "$out/rdma-preflight.log"
restore() {
 rc=$?
 trap - EXIT
 echo "RESTORE installed direct/TCP; gate runner exit=$rc"
 EFS_CLUSTER=amd EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp "$base/devops/start.sh" 9>&- > "$out/restoration.log" 2>&1 || { cat "$out/restoration.log"; exit 90; }
 echo "$rc" > "$out/runner-exit"
 exit "$rc"
}
trap restore EXIT
for m in "$base/mnt" "$base/mnt2"; do
 if ! "$base/src/scripts/client.sh" stop "$m"; then
  mountpoint -q "$m" && exit 1
  sleep 1
  pgrep -f "^./efs-fuse .* $m$" && exit 1
 fi
done
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)')
[ "$(echo "$pids" | wc -l)" = 4 ]
kill -TERM $pids
for i in $(seq 1 60); do alive=0; for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done; [ "$alive" = 0 ] && break; sleep 1; done
[ "$alive" = 0 ] || exit 1
failed=0
for mode in direct buffered; do
 for gate in mount-sync full; do
  echo "START $mode RXE $gate $(date -u +%FT%TZ)"
  args=(--mode "$mode" --transport rdma --root "$base" --root "$base" --mount "$base/mnt" --peer-mount "$base/mnt2")
  if [ "$gate" = mount-sync ]; then args+=(--mount-sync); else
   args+=(--posix --continue-regression-failure --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race --append-audit --stop-dirty-recovery --shutdown-mutations)
  fi
  if python3 "$build/tests/live/gc_reclamation.py" "${args[@]}" > "$out/$mode-$gate.log" 2>&1; then
   echo PASS > "$out/$mode-$gate.status"
  else echo FAIL > "$out/$mode-$gate.status"; failed=1; fi
  tail -n 15 "$out/$mode-$gate.log"
  mountpoint -q "$base/mnt" && exit 1
  mountpoint -q "$base/mnt2" && exit 1
 done
done
exit "$failed"
