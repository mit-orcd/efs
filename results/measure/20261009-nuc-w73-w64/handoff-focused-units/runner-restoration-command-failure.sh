#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/w73-w64-gates-v2.lock
flock 9
build=/data1/efs/gate-closure-build
out=/data1/efs/w73-w64-handoff-fix-de2f8b286694
mkdir -p "$out"
restore() {
 rc=$?
 trap - EXIT
 echo "RESTORE installed service; gate runner exit=$rc"
 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp -u /data1/efs/devops/start.sh 9>&- >"$out/restoration.log" 2>&1 || { cat "$out/restoration.log"; exit 90; }
 echo "$rc" > "$out/runner-exit"
 exit "$rc"
}
# Build and run units/replay only after the previous runner released its lock.
cd "$build"
cp -u /data1/efs/append-candidate-input/raft_host.c src/server/raft_host.c
cp -u /data1/efs/append-candidate-input/efs_fuse.c src/client/efs_fuse.c
cp -u /data1/efs/append-candidate-input/kv_key.h /data1/efs/append-candidate-input/meta_cmd.h include/efs/
cp -u /data1/efs/append-candidate-input/replay_close_cleanup.py /data1/efs/append-candidate-input/inspect_close_cleanup.c tests/live/
cp -u /data1/efs/truncate-candidate-input/raft_host.c src/server/
cp -u /data1/efs/truncate-candidate-input/efs_fuse.c src/client/
cp -u /data1/efs/truncate-candidate-input/meta_apply.c src/meta/
cp -u /data1/efs/truncate-candidate-input/test_meta_apply.c /data1/efs/truncate-candidate-input/test_server_processes.py tests/
cp -u /data1/efs/truncate-candidate-input/gc_reclamation.py /data1/efs/truncate-candidate-input/replay_close_cleanup.py /data1/efs/truncate-candidate-input/inspect_close_cleanup.c tests/live/
cp -u /data1/efs/handoff-candidate-input/efs_fuse.c src/client/
cp -u /data1/efs/handoff-candidate-input/gc_reclamation.py tests/live/
make -j8 efsd efs-fuse efs-mgmt test EFS_GIT_ID=04e9d616-handoff-de2f8b286694 EFS_GIT_DESCRIBE=append-handoff-test EFS_GIT_BRANCH=devel-gate-closure > "$out/build-units.log" 2>&1
cc -Iinclude -Isrc/common tests/live/inspect_close_cleanup.c libefs.a -lpthread -lm -ldl -libverbs -o /data1/efs/inspect-close-cleanup
python3 tests/live/replay_close_cleanup.py --fixture /data1/efs/gc-direct-agr8e5h7 --blocked-index 14029 --resolution-index 14040 --inode 5516 --inspector /data1/efs/inspect-close-cleanup > "$out/replay.log" 2>&1
trap restore EXIT
# Drain installed mounts without disposable-cluster force-discard behavior.
for m in /data1/efs/mnt /data1/efs/mnt2; do
 if ! /data1/efs/src/scripts/client.sh stop "$m"; then
  mountpoint -q "$m" && exit 1
  sleep 1
  pgrep -f "^./efs-fuse .* $m$" && exit 1
 fi
done
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)')
kill -TERM $pids
for i in $(seq 1 60); do alive=0; for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done; [ "$alive" = 0 ] && break; sleep 1; done
[ "$alive" = 0 ] || exit 1
trap restore EXIT
export EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_TRANSPORT=tcp
python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp --root /data1/efs --root /data2/efs --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --append-truncate-only > "$out/focused-append-truncate.log" 2>&1
failed=0
for mode in direct buffered; do
 echo "START $mode TCP $(date -u +%FT%TZ)"
 if python3 "$build/tests/live/gc_reclamation.py" --mode "$mode" --transport tcp --root /data1/efs --root /data2/efs --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --posix --continue-posix-failure --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race --append-audit --stop-dirty-recovery --shutdown-mutations > "$out/$mode-tcp.log" 2>&1; then
  echo PASS > "$out/$mode-tcp.status"
 else
  echo FAIL > "$out/$mode-tcp.status"; failed=1
 fi
 tail -20 "$out/$mode-tcp.log"
 mountpoint -q /data1/efs/mnt && exit 1
 mountpoint -q /data1/efs/mnt2 && exit 1
done
exit "$failed"
