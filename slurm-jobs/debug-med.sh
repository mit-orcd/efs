#!/bin/bash
#SBATCH --job-name=efs-dbg
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:10:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/debug-med-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/debug-med-%j.err
set -uo pipefail
REPO=/home/erbmi1/git/efs
if [ ! -d /scratch ]; then
  echo "ERROR: /scratch required on compute node"
  exit 1
fi
LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
rm -rf "$LOCAL"
mkdir -p "$LOCAL"
echo "host=$(hostname) LOCAL=$LOCAL"
rm -rf "$LOCAL"
mkdir -p "$LOCAL"/{s1,s2,s3,mnt}
IP=127.0.0.1
"$REPO/efsd" --node-id 1 --addr "$IP" --port 1991 --storage "$LOCAL/s1" >"$LOCAL/s1.log" 2>&1 &
S1=$!
for i in $(seq 1 40); do grep -q listening "$LOCAL/s1.log" 2>/dev/null && break; sleep 0.25; done
grep listening "$LOCAL/s1.log" || { echo s1_fail; cat "$LOCAL/s1.log"; exit 1; }
"$REPO/efsd" --node-id 2 --addr "$IP" --port 1992 --storage "$LOCAL/s2" --join "$IP:1991" >"$LOCAL/s2.log" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port 1993 --storage "$LOCAL/s3" --join "$IP:1991" >"$LOCAL/s3.log" 2>&1 &
S3=$!
sleep 2
"$REPO/efs-mgmt" mkfs "$IP:1991" dbgexport; echo mkfs_rc=$?
EFS_META_BATCH_OPS=4096 "$REPO/efs-fuse" "$IP:1991" "$IP:1992" "$IP:1993" dbgexport "$LOCAL/mnt" -f >"$LOCAL/client.log" 2>&1 &
CPID=$!
for i in $(seq 1 40); do
  mountpoint -q "$LOCAL/mnt" && break
  kill -0 "$CPID" 2>/dev/null || { echo client_died; cat "$LOCAL/client.log"; exit 1; }
  sleep 0.25
done
if ! mountpoint -q "$LOCAL/mnt"; then echo mount_fail; cat "$LOCAL/client.log"; exit 1; fi
echo mounted
mkdir -p "$LOCAL/mnt/tree"; echo mkdir_rc=$?
echo hi > "$LOCAL/mnt/tree/f0"; echo write_rc=$?
ls -la "$LOCAL/mnt/tree" | head
for i in $(seq 1 5000); do
  printf 'x' > "$LOCAL/mnt/tree/f$i" || { echo "fail_at=$i"; break; }
  if (( i % 1000 == 0 )); then echo at_$i; fi
done
echo stress_done
fusermount -u "$LOCAL/mnt" || true
sleep 1
kill -INT $CPID $S1 $S2 $S3 2>/dev/null || true
wait $CPID 2>/dev/null; echo client_exit=$?
kill -KILL $CPID $S1 $S2 $S3 2>/dev/null || true
echo DONE
tail -30 "$LOCAL/client.log" || true
tail -10 "$LOCAL/s1.log" || true
