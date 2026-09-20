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
#   tests/perf/io500/run.sh ior-hard-write <segs>   # raw IOR hard geometry, -k keep
#   tests/perf/io500/run.sh ior-hard-verify <segs>  # -r -R the same file (cold-remount first)
#   tests/perf/io500/run.sh ior-easy-write <mb>     # raw IOR easy geometry (file-per-proc), -k keep
#   tests/perf/io500/run.sh ior-easy-verify <mb>    # -r -R -C -Q 1 (rank reads a file from 2 nodes away)
#
# Env:
#   IO500_DIR   build tree (default $HOME/orcd/scratch/efs-io500)
#   EFS_MNT     FUSE mount (default /tmp/efs-mount)
#   RANK0       MPI launch host (default fcstor007.ib)
#   SLOTS       ranks per client (default 1 → 9 ranks)
#   NP          total ranks (default: 9 * SLOTS)
#   STONEWALL   override [debug] stonewall-time in the generated ini
#   MPIRUN      launcher (default: mpirun)
#   MPIVARS     optional path to DOCA-OFED mpivars.sh
set -euo pipefail

SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
export EFS_SSH_TIMEOUT="${EFS_SSH_TIMEOUT:-400}"
HERE="$(cd "$(dirname "$0")" && pwd)"
IO500_DIR=${IO500_DIR:-$HOME/orcd/scratch/efs-io500}
SRC="$IO500_DIR/io500"
EFS_MNT=${EFS_MNT:-/tmp/efs-mount}
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
    # Detach real runs: a foreground prterun dies with the wrapper's
    # EFS_SSH_TIMEOUT and the driver stdout is lost (the job itself
    # survives). The log lands on the NFS tree — tail it from the login
    # node: tail -f $IO500_DIR/last-run.log. dry-run stays foreground.
    local detach=1
    case " ${extra[*]+"${extra[*]}"} " in *" --dry-run "*) detach=0 ;; esac
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
        cd '$SRC'
        if [ $detach = 1 ]; then
            setsid $MPIRUN --hostfile '$IO500_DIR/hosts' -np $NP \
            --prefix \"\$(dirname \"\$(dirname \"\$(command -v mpicc)\")\")\" \
            -x PATH -x LD_LIBRARY_PATH \
            --mca plm_rsh_agent '$HERE/mpi-ssh.sh' \
            --mca plm_rsh_no_tree_spawn 1 \
            ./io500 '$ini' ${extra[*]+"${extra[*]}"} >'$IO500_DIR/last-run.log' 2>&1 </dev/null &
            echo \"started; log: $IO500_DIR/last-run.log\"
        else
            $MPIRUN --hostfile '$IO500_DIR/hosts' -np $NP \
            --prefix \"\$(dirname \"\$(dirname \"\$(command -v mpicc)\")\")\" \
            -x PATH -x LD_LIBRARY_PATH \
            --mca plm_rsh_agent '$HERE/mpi-ssh.sh' \
            --mca plm_rsh_no_tree_spawn 1 \
            ./io500 '$ini' ${extra[*]+"${extra[*]}"}
        fi"
}

# Drive IOR hard-mode geometry directly (io500's bin/ior), bypassing the
# driver's unconditional end-of-run purge so the same file can be verified
# from cold-remounted clients. Fixed -G signature makes a separate read
# invocation's -R check meaningful. Detached like launch().
IOR_HARD_FILE=${IOR_HARD_FILE:-$EFS_MNT/io500/hardv/file}
launch_ior_hard() { # write|verify segments
    local mode=$1 segs=$2 flags
    case "$mode" in
        write)  flags="-w" ;;
        verify) flags="-r -R" ;;
        *) echo "run: launch_ior_hard write|verify" >&2; return 2 ;;
    esac
    remote "test -x '$SRC/bin/ior' || { echo 'run: missing $SRC/bin/ior' >&2; exit 1; }"
    remote "grep -q 'efs-fuse $EFS_MNT ' /proc/mounts || { echo 'run: $EFS_MNT not mounted on $RANK0' >&2; exit 1; }"
    remote "mkdir -p '$(dirname "$IOR_HARD_FILE")'"
    gen_hostfile "$SLOTS" > "$IO500_DIR/hosts"
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
        cd '$SRC'
        setsid $MPIRUN --hostfile '$IO500_DIR/hosts' -np $NP \
        --prefix \"\$(dirname \"\$(dirname \"\$(command -v mpicc)\")\")\" \
        -x PATH -x LD_LIBRARY_PATH \
        --mca plm_rsh_agent '$HERE/mpi-ssh.sh' \
        --mca plm_rsh_no_tree_spawn 1 \
        ./bin/ior -a POSIX -C -Q 1 -g -G=271828 -k -e -t 47008 -b 47008 -s $segs \
            $flags -o '$IOR_HARD_FILE' >'$IO500_DIR/last-run.log' 2>&1 </dev/null &
        echo \"started ior-hard $mode segs=$segs; log: $IO500_DIR/last-run.log\""
}

# Same for ior-easy geometry (file-per-proc, 1 MiB transfers, -C -Q 1 so
# rank i reads the file rank i+2*slots wrote on another node). <mb> is the
# per-rank file size in MiB.
IOR_EASY_DIR=${IOR_EASY_DIR:-$EFS_MNT/io500/easyv}
launch_ior_easy() { # write|verify mb
    local mode=$1 mb=$2 flags
    case "$mode" in
        write)  flags="-w" ;;
        verify) flags="-r -R" ;;
        *) echo "run: launch_ior_easy write|verify" >&2; return 2 ;;
    esac
    remote "test -x '$SRC/bin/ior' || { echo 'run: missing $SRC/bin/ior' >&2; exit 1; }"
    remote "grep -q 'efs-fuse $EFS_MNT ' /proc/mounts || { echo 'run: $EFS_MNT not mounted on $RANK0' >&2; exit 1; }"
    remote "mkdir -p '$IOR_EASY_DIR'"
    gen_hostfile "$SLOTS" > "$IO500_DIR/hosts"
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
        cd '$SRC'
        setsid $MPIRUN --hostfile '$IO500_DIR/hosts' -np $NP \
        --prefix \"\$(dirname \"\$(dirname \"\$(command -v mpicc)\")\")\" \
        -x PATH -x LD_LIBRARY_PATH \
        --mca plm_rsh_agent '$HERE/mpi-ssh.sh' \
        --mca plm_rsh_no_tree_spawn 1 \
        ./bin/ior -a POSIX -F -C -Q 1 -g -G=271828 -k -e -t 1m -b ${mb}m \
            $flags -o '$IOR_EASY_DIR/ior_file_easy' >'$IO500_DIR/last-run.log' 2>&1 </dev/null &
        echo \"started ior-easy $mode mb=$mb; log: $IO500_DIR/last-run.log\""
}

case "$cmd" in
    ior-hard-write|ior-hard-verify)
        launch_ior_hard "${cmd#ior-hard-}" "${1:?segments}"
        ;;
    ior-easy-write|ior-easy-verify)
        launch_ior_easy "${cmd#ior-easy-}" "${1:?mb}"
        ;;
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
