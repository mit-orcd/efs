#!/bin/bash
#SBATCH --job-name=efs-memb
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:10:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=2G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/memb-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/memb-%j.err
#
# Reproduce the old nodes[0]-smash bug: persist membership with local not at
# index 0, restart, confirm three distinct ids remain.

set -euo pipefail
REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
SCRATCH=/scratch/efs-testing/${SLURM_JOB_ID}
IP=127.0.0.1
P1=19201
P2=19202
P3=19203

mkdir -p "$SHARED/logs" "$SCRATCH"/{s1,s2,s3}/log
cd "$REPO"
cleanup() {
    set +e
    for p in "$SCRATCH"/s*/log/efsd.pid; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
    done
    sleep 1
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

make -j"$(nproc)" efsd efs-mgmt

./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$SCRATCH/s1" --writers 1 \
    >"$SCRATCH/s1.log" 2>&1 &
echo $! >"$SCRATCH/s1/log/efsd.pid"
sleep 1
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$SCRATCH/s2" --writers 1 \
    --join "$IP:$P1" >"$SCRATCH/s2.log" 2>&1 &
echo $! >"$SCRATCH/s2/log/efsd.pid"
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$SCRATCH/s3" --writers 1 \
    --join "$IP:$P1" >"$SCRATCH/s3.log" 2>&1 &
echo $! >"$SCRATCH/s3/log/efsd.pid"
sleep 2

./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/before.txt"
grep -c 'node 1:' "$SCRATCH/before.txt" | grep -qx 1
grep -c 'node 2:' "$SCRATCH/before.txt" | grep -qx 1
grep -c 'node 3:' "$SCRATCH/before.txt" | grep -qx 1

# Restart node 3 (historically smashed nodes[0] into a duplicate of itself).
kill "$(cat "$SCRATCH/s3/log/efsd.pid")"
sleep 1
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$SCRATCH/s3" --writers 1 \
    --join "$IP:$P1" >"$SCRATCH/s3b.log" 2>&1 &
echo $! >"$SCRATCH/s3/log/efsd.pid"
sleep 2

./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/after.txt"
# Must still see three distinct node ids, no duplicates.
python3 - <<'PY' "$SCRATCH/after.txt"
import re, sys
text = open(sys.argv[1]).read()
ids = re.findall(r'^\s+node (\d+):', text, re.M)
print("ids", ids)
assert len(ids) == 3, ids
assert len(set(ids)) == 3, ids
assert "WARNING: duplicate" not in text
print("MEMBERSHIP_OK")
PY

echo "MEMBERSHIP_SMOKE_OK"
