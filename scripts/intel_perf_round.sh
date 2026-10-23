#!/bin/bash
# intel_perf_round.sh OUT [--no-deploy]
# One performance round on the Intel cluster: deploy this checkout's HEAD to
# intel1/intel2 (devops nuc-efs deploy/stop/start, storage kept) and build
# the matching efs-fuse for the xorinox client, remount, then run the
# tiny-file sweep (WORKERS, default 8 16 32; RUNSEC s each) without perf
# attached. Writes OUT/sweep.tsv (workers, files, files/s) and keeps the
# client's EFS_OPID_STATS lines per case. Prerequisites: devops at
# ~/git/devops, SSH aliases intel1_efs intel2_efs xorinox_efs.
set -eu
OUT=$1; DEPLOY=1; [ "${2:-}" = --no-deploy ] && DEPLOY=0
WORKERS=${WORKERS:-8 16 32}; RUNSEC=${RUNSEC:-45}
SRC=$(git rev-parse --show-toplevel); ID=$(git rev-parse --short=12 HEAD)
DEVOPS=${DEVOPS:-$HOME/git/devops/nuc-efs}; CB=/data1/efs/intel-client
MNT=/data1/efs/intel-mnt; LOG=$CB/logs/efs-fuse-intel-mnt.log
mkdir -p "$OUT"; log(){ echo "$(date -u +%FT%TZ) $*" | tee -a "$OUT/run.log"; }
if [ $DEPLOY = 1 ]; then
  git -C "$SRC" diff --quiet HEAD || { echo "uncommitted changes in $SRC" >&2; exit 2; }
  log "deploy $ID"
  ssh xorinox_efs "fuser -m $MNT >/dev/null 2>&1 && { echo busy; exit 1; }; $CB/client.sh stop" >> "$OUT/run.log" 2>&1
  (cd "$DEVOPS" && EFS_CLUSTER=intel ./stop.sh) >> "$OUT/run.log" 2>&1
  (cd "$DEVOPS" && EFS_CLUSTER=intel ./deploy.sh "$SRC/") > "$OUT/deploy.log" 2>&1
  (cd "$DEVOPS" && EFS_CLUSTER=intel EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 ./start.sh) >> "$OUT/run.log" 2>&1
  BT=$(ssh intel1_efs '/data1/efs/src/efs-fuse --version' | sed -n 's/.*built \([^ ]*\) on.*/\1/p')
  DESC=$(ssh intel1_efs '/data1/efs/src/efs-fuse --version' | awk '{print $2}')
  git -C "$SRC" archive --format=tar HEAD | ssh xorinox_efs "set -e; R=$CB/releases/$ID; rm -rf \$R; mkdir -p \$R; tar -xf - -C \$R; cd \$R; make -j8 -B EFS_GIT_ID=$ID EFS_GIT_DESCRIBE=$DESC EFS_GIT_BRANCH=devel-performance EFS_BUILD_TIME=$BT efs-fuse efs-mgmt > build.log 2>&1; ln -sfn \$R $CB/src"
  ssh xorinox_efs "EFS_OPID_STATS=1 EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 $CB/client.sh start" >> "$OUT/run.log" 2>&1
  log "deployed: $(ssh xorinox_efs "$CB/src/efs-fuse --version")"
fi
printf 'workers\tfiles\tfiles_per_s\n' > "$OUT/sweep.tsv"
for w in $WORKERS; do
  mark=$(ssh xorinox_efs "wc -l < $LOG")
  # Disk activity mid-run: 60 s of iostat on both servers (w/s, f/s, util).
  for h in intel1_efs intel2_efs; do
    ssh $h "sleep 30; iostat -dx 60 2" 2>/dev/null | awk -v h=$h '/^Device/{n++} n==2 && /^nvme/{print h, $1, "w/s=" $8, "f/s=" $(NF-3), "w_await=" $12, "util=" $NF}' > "$OUT/iostat-w$w-$h.txt" &
  done
  n=$(ssh xorinox_efs "d=$MNT/round-$ID-w$w-\$\$; timeout -s KILL $RUNSEC python3 $CB/tiny_files.py \$d --max-files 5000000 --workers $w --max-depth 2 >/dev/null 2>&1; find \$d -type f | wc -l")
  wait
  printf '%s\t%s\t%s\n' $w $n $(python3 -c "print(round($n/$RUNSEC,1))") >> "$OUT/sweep.tsv"
  ssh xorinox_efs "tail -n +$((mark+1)) $LOG | grep 'opid-stats' || true" > "$OUT/opid-w$w.txt"
  log "workers=$w files=$n"
done
cat "$OUT/sweep.tsv"; cat "$OUT"/iostat-*.txt 2>/dev/null
