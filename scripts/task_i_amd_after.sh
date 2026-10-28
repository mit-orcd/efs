#!/usr/bin/env bash
# Wait for an earlier AMD controller to close its evidence before a fresh run.
# PREVIOUS_EVIDENCE COMMIT NEW_EVIDENCE_ROOT
set -u
previous=${1:?previous evidence required}
commit=${2:?exact source commit required}
art=${3:?fresh evidence root required}
[[ "$(hostname -s)" == amd-* && "$(pwd -P)" == "$HOME"/efs-taskI/*/src ]] || exit 2
[[ "$previous" == "$HOME"/efs-artifacts/taskI-amd-* && "$art" == "$HOME"/efs-artifacts/taskI-amd-* ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
echo WAIT_PREVIOUS > "$art/stage"
end=$((SECONDS+10800))
while [ "$SECONDS" -lt "$end" ]; do
 if [ "$(cat "$previous/stage" 2>/dev/null)" == FINISHED ] && [ -s "$previous/controller-exit" ]; then
  break
 fi
 sleep 20
done
[ "$(cat "$previous/stage" 2>/dev/null)" == FINISHED ] && [ -s "$previous/controller-exit" ] || exit 124
cp "$previous/controller-exit" "$art/previous-exit"
echo QUALIFICATION > "$art/stage"
# The child still requires two quiet all-account snapshots. Failed prior
# results remain failures, even when the fresh candidate eventually passes.
bash scripts/task_i_amd.sh "$commit" "$art/qualification"
rc=$?
echo FINISHED > "$art/stage"
exit "$rc"
