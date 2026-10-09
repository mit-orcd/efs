#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/w73-w64-gates-v2.lock
flock 9
base=/data1/efs
release=$base/gate-closure-release-393f5bae
out=$base/deploy-393f5bae
mkdir "$out"
[ "$(cat "$base/w73-w64-handoff-fix-de2f8b286694/runner-exit")" = 0 ]
[ -s "$release/release-binary-sha256.txt" ]
(cd "$release" && sha256sum -c release-binary-sha256.txt)
[ "$(cat "$release/.build_id.stamp")" = 393f5bae2c00 ]
backup=$base/src-pre-393f5bae-$(date -u +%Y%m%dT%H%M%SZ)
echo "$backup" > "$out/source-backup.txt"
restore() {
 rc=$?
 trap - EXIT
 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp "$base/devops/start.sh" 9>&- >"$out/start.log" 2>&1 || { cat "$out/start.log"; exit 90; }
 echo "$rc" > "$out/deploy-exit"
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
mv "$base/src" "$backup"
mv "$release" "$base/src"
echo 'Installed committed source and clean release binaries'
