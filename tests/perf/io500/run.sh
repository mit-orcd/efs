#!/bin/bash
# Launch the IO-500 driver (IOR phases) across the 9 efs-fuse clients.
# Orchestrates from the login node via efs-ssh; MPI starts on RANK0.
# Does not use Slurm.
#
# Usage:
#   tests/perf/io500/run.sh prereqs          # check git/gcc/mpicc/mpirun
#   tests/perf/io500/run.sh prepare          # clone+build on RANK0 (NFS tree)
#   tests/perf/io500/run.sh debug            # 1s stonewall IOR smoke
#   tests/perf/io500/run.sh ior              # 30s stonewall IOR easy+hard
#   tests/perf/io500/run.sh dry-run debug    # print io500 argv only
#
# Env:
#   IO500_DIR   build tree (default $HOME/orcd/scratch/efs-io500)
#   EFS_MNT     FUSE mount (default /tmp/efs/mnt)
#   RANK0       MPI launch host (default fcstor007.ib)
#   SLOTS       ranks per client (default 1 → 9 ranks)
#   NP          total ranks (default: 9 * SLOTS)
#   STONEWALL   override [debug] stonewall-time in the generated ini
#   MPIRUN      launcher (default: mpirun)
#   MPIVARS     optional path to DOCA-OFED mpivars.sh
set -euo pipefail

SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
HERE="$(cd "$(dirname "$0")" && pwd)"
IO500_DIR=${IO500_DIR:-$HOME/orcd/scratch/efs-io500}
SRC="$IO500_DIR/io500"
EFS_MNT=${EFS_MNT:-/tmp/efs/mnt}
RANK0=${RANK0:-fcstor007.ib}
SLOTS=${SLOTS:-1}
NP=${NP:-$((9 * SLOTS))}
MPIRUN=${MPIRUN:-mpirun}
MPI_BOOT=$(cat "$HERE/mpi-env.sh")

cmd=${1:-}
shift || true

remote() {
    "$SSH" "$RANK0" "export IO500_DIR='$IO500_DIR' EFS_MNT='$EFS_MNT' MPIVARS='${MPIVARS:-}'
$MPI_BOOT
$*"
}

gen_hostfile() {
    local slots=$1
    sed -E "s/slots=[0-9]+/slots=$slots/" "$HERE/hosts"
}

prep_ini() { # debug|ior  → prints remote ini path
    local kind=$1
    local srcini
    case "$kind" in
        debug) srcini="$HERE/config-debug.ini" ;;
        ior)   srcini="$HERE/config-ior-only.ini" ;;
        *) echo "run: unknown config '$kind'" >&2; return 2 ;;
    esac
    local stamp
    stamp=$(date -u +%Y%m%d-%H%M%S)
    local dst="$SRC/efs-$kind-$stamp.ini"
    # Rewrite datadir onto the live mount; optional stonewall override.
    awk -v mnt="$EFS_MNT" -v sw="${STONEWALL:-}" '
        /^datadir/ { print "datadir = " mnt "/io500"; next }
        /^resultdir/ { print "resultdir = " mnt "/io500-results"; next }
        /^stonewall-time/ && sw != "" { print "stonewall-time = " sw; next }
        { print }
    ' "$srcini" > "/tmp/efs-io500-$kind.ini"
    remote "mkdir -p '$SRC' '$EFS_MNT/io500' '$EFS_MNT/io500/ior-easy' \
        '$EFS_MNT/io500/ior-hard' '$EFS_MNT/io500-results'"
    # NFS home: the ini is visible on RANK0 if we write it under IO500_DIR.
    cp "/tmp/efs-io500-$kind.ini" "$dst"
    gen_hostfile "$SLOTS" > "$IO500_DIR/hosts"
    echo "$dst"
}

launch() { # extra-args...  ini-path
    local extra=()
    while [ $# -gt 1 ]; do extra+=("$1"); shift; done
    local ini=$1
    remote "test -x '$SRC/io500' || { echo 'run: missing $SRC/io500 — run prepare first' >&2; exit 1; }"
    remote "grep -q 'efs-fuse $EFS_MNT ' /proc/mounts || { echo 'run: $EFS_MNT not mounted on $RANK0' >&2; exit 1; }"
    # OpenMPI: same binary path on all ranks (NFS tree). SSH between clients.
    # --prefix + -x so remote ranks find libmpi/libpmix (same DOCA-OFED tree).
    remote "export LD_LIBRARY_PATH=\"\${LD_LIBRARY_PATH:-}\"
        if ! { [ -f /tmp/efs-io500-agent.env ] && . /tmp/efs-io500-agent.env && ssh-add -l 2>/dev/null | grep -q efs-test; }; then
            ssh-agent -s > /tmp/efs-io500-agent.env
            chmod 600 /tmp/efs-io500-agent.env
            . /tmp/efs-io500-agent.env
            DISPLAY=\"\${DISPLAY:-:0}\" SSH_ASKPASS=\"\$HOME/.cursor/secrets/efs-test/askpass.sh\" \
                setsid -w ssh-add \"\$HOME/.cursor/secrets/efs-test/id_ed25519\" </dev/null >/dev/null
        else
            . /tmp/efs-io500-agent.env
        fi
        cd '$SRC' && $MPIRUN --hostfile '$IO500_DIR/hosts' -np $NP \
        --prefix \"\$(dirname \"\$(dirname \"\$(command -v mpicc)\")\")\" \
        -x PATH -x LD_LIBRARY_PATH \
        --mca plm_rsh_agent '$HERE/mpi-ssh.sh' \
        --mca plm_rsh_no_tree_spawn 1 \
        ./io500 ${extra[*]+"${extra[*]}"} '$ini'"
}

case "$cmd" in
    prereqs)
        remote "bash -s" <"$HERE/check-prereqs.sh"
        ;;
    prepare)
        remote "bash -s" <"$HERE/prepare.sh"
        ;;
    dry-run)
        kind=${1:?dry-run needs debug|ior}
        ini=$(prep_ini "$kind")
        launch --dry-run "$ini"
        ;;
    debug|ior)
        ini=$(prep_ini "$cmd")
        launch "$ini"
        ;;
    *)
        sed -n '2,22p' "$0"
        exit 2
        ;;
esac
