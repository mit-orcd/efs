#!/bin/bash
#SBATCH --job-name=efs-gdb
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:10:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/debug-gdb-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/debug-gdb-%j.err
set -uo pipefail
REPO=/home/erbmi1/git/efs
LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
rm -rf "$LOCAL"; mkdir -p "$LOCAL"/{s1,s2,s3,mnt,gdb}
IP=127.0.0.1
"$REPO/efsd" --node-id 1 --addr "$IP" --port 1991 --storage "$LOCAL/s1" >"$LOCAL/s1.log" 2>&1 &
S1=$!
sleep 1
"$REPO/efsd" --node-id 2 --addr "$IP" --port 1992 --storage "$LOCAL/s2" --join "$IP:1991" >"$LOCAL/s2.log" 2>&1 &
"$REPO/efsd" --node-id 3 --addr "$IP" --port 1993 --storage "$LOCAL/s3" --join "$IP:1991" >"$LOCAL/s3.log" 2>&1 &
sleep 2
"$REPO/efs-mgmt" mkfs "$IP:1991" dbgexport
# Run fuse under gdb, batch commands
cat > "$LOCAL/gdb.cmd" <<'G'
set pagination off
run
bt
info registers
quit
G
EFS_META_BATCH_OPS=4096 gdb -batch -x "$LOCAL/gdb.cmd" --args \
  "$REPO/efs-fuse" "$IP:1991" "$IP:1992" "$IP:1993" dbgexport "$LOCAL/mnt" -f \
  >"$LOCAL/gdb.out" 2>&1 &
GPID=$!
for i in $(seq 1 60); do mountpoint -q "$LOCAL/mnt" && break; sleep 0.25; done
echo mount=$(mountpoint -q "$LOCAL/mnt" && echo yes || echo no)
# trigger ops in background while gdb runs fuse
( mkdir -p "$LOCAL/mnt/tree"; echo hi > "$LOCAL/mnt/tree/f0"; for i in $(seq 1 100); do echo x > "$LOCAL/mnt/tree/f$i"; done; echo ops_done ) >"$LOCAL/ops.log" 2>&1 &
OPID=$!
# wait for segfault or ops
for i in $(seq 1 40); do
  kill -0 "$GPID" 2>/dev/null || break
  kill -0 "$OPID" 2>/dev/null || break
  sleep 0.5
done
wait "$OPID" 2>/dev/null || true
sleep 2
kill -INT "$GPID" 2>/dev/null || true
wait "$GPID" 2>/dev/null || true
echo '=== ops ==='; cat "$LOCAL/ops.log"
echo '=== gdb (tail) ==='; tail -80 "$LOCAL/gdb.out"
# copy gdb out to shared
cp -f "$LOCAL/gdb.out" /orcd/scratch/orcd/001/erbmi1/efs/logs/debug-gdb-${SLURM_JOB_ID}.gdb.txt
kill -KILL $S1 2>/dev/null || true
pkill -f "efsd --node-id" 2>/dev/null || true
