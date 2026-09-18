#!/bin/bash
# One-shot IOR-hard write-check (-W). Not mdtest -W (that is stonewall).
# 9 ranks, 1 s stonewall, 47008 transfer (do not retune).
set -euo pipefail
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
HERE=/home/erbmi1/git/efs/tests/perf/io500
IO500_DIR=${IO500_DIR:-$HOME/orcd/scratch/efs-io500}
SRC="$IO500_DIR/io500"
EFS_MNT=/tmp/efs-mount
RANK0=fcstor007.ib
STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="$EFS_MNT/io500/hard-w-$STAMP"
MPI_BOOT=$(cat "$HERE/mpi-env.sh")
"$SSH" "$RANK0" "export IO500_DIR='$IO500_DIR' EFS_MNT='$EFS_MNT'
$MPI_BOOT
mkdir -p '$OUTDIR'
if ! { [ -f /tmp/efs-io500-agent.env ] && . /tmp/efs-io500-agent.env && ssh-add -l 2>/dev/null | grep -q efs-test; }; then
    ssh-agent -s > /tmp/efs-io500-agent.env
    chmod 600 /tmp/efs-io500-agent.env
    . /tmp/efs-io500-agent.env
    DISPLAY=\"\${DISPLAY:-:0}\" SSH_ASKPASS=\"\$HOME/.cursor/secrets/efs-test/askpass.sh\" \
        setsid -w ssh-add \"\$HOME/.cursor/secrets/efs-test/id_ed25519\" </dev/null >/dev/null
else
    . /tmp/efs-io500-agent.env
fi
cd '$SRC' && mpirun --hostfile '$IO500_DIR/hosts' -np 9 \
    --prefix \"\$(dirname \"\$(dirname \"\$(command -v mpicc)\")\")\" \
    -x PATH -x LD_LIBRARY_PATH \
    --mca plm_rsh_agent '$HERE/mpi-ssh.sh' \
    --mca plm_rsh_no_tree_spawn 1 \
    ./bin/ior --dataPacketType=timestamp -C -Q 1 -g -G=1904173133 -k -e \
    -o '$OUTDIR/file' -t 47008 -b 47008 -s 100000 \
    -w -W -D 1 -a POSIX -O stoneWallingWearOut=1"
