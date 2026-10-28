#!/usr/bin/env bash
# Bounded quiet-host handoff without signalling another session's jobs.
# COMMIT NEW_EVIDENCE_ROOT; invoked from an exported Task I candidate.
set -u
commit=${1:?exact source commit required}
art=${2:?fresh evidence root required}
[[ "$(pwd -P)" == "$HOME"/efs-taskI/*/src && "$commit" =~ ^[0-9a-f]{9,40}$ ]] || exit 2
wait_art="$art-wait"
mkdir "$wait_art" || exit 2
exec >"$wait_art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$wait_art/controller-exit"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
echo WAIT_QUIET > "$wait_art/stage"
end=$((SECONDS+14400));quiet=0
while [ "$SECONDS" -lt "$end" ]; do
 # Task O has staged stress/load phases: a momentary process gap is not a
 # handoff while its detached controllers remain live. Never stop them.
 screens=$(SCREENDIR="$HOME/.screen" screen -ls 2>/dev/null || :)
 if [[ "$screens" == *taskO-* ]]; then quiet=0;sleep 20;continue;fi
 python3 - "$wait_art" <<'PY'
import json,sys,time
from pathlib import Path
sys.path.insert(0,'scripts')
from task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
(Path(sys.argv[1])/'last-check.json').write_text(json.dumps({'time':time.time(),'jobs':jobs},indent=2)+'\n')
raise SystemExit(1 if jobs else 0)
PY
 if [ "$?" -eq 0 ]; then quiet=$((quiet+1));else quiet=0;fi
 if [ "$quiet" -ge 2 ]; then
  echo HANDOFF > "$wait_art/stage"
  bash scripts/task_i_candidate.sh "$commit" "$art"
  rc=$?;echo FINISHED > "$wait_art/stage";exit "$rc"
 fi
 sleep 20
done
echo DEADLINE > "$wait_art/stage"
exit 124
