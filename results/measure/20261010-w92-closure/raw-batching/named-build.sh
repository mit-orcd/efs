#!/usr/bin/env bash
set -euo pipefail
base=/mnt/w92-scale-big-20261010
# The first named rebuild is already live; its wrapper's quoting made its
# exit marker unreliable. Wait for that make, then require a real make verdict.
while kill -0 574971 2>/dev/null; do sleep 5; done
cd "$base/build-cdc13b88"
set +e
make -j2 EFS_GIT_ID=cf4e365a EFS_GIT_DESCRIBE=cf4e365a \
 EFS_GIT_BRANCH=devel-find-stats efsd efs-fuse efs-mgmt \
 > "$base/w92-build-cf4e365a-verified.log" 2>&1
w92_build_rc=$?
printf '%s\n' "$w92_build_rc" > "$base/w92-build-cf4e365a-verified.exit"
date -u > "$base/w92-build-cf4e365a-verified.finished"
exit "$w92_build_rc"
