#!/bin/bash
# Run ON node9900 (detached, in screen): orcd-mini batch O rows O8 (real-HCA
# TSan and ASan/UBSan gates, K49/K51) and O9 (Memcheck leak gate, K26).
# Usage: orcd_mini_o8_o9.sh RELEASE_DIR OUTDIR
# Copies RELEASE_DIR to a fresh /tmp tree (deleted at the end), runs
#   make tsan-check; make asan-check; make leak-check
# with the RDMA device, the private libfuse and, for Memcheck, the
# user-prefix Valgrind 3.28.0 on the shared home (EFS_VG_PREFIX, default
# $HOME/.local/valgrind-3.28.0; docs/status/2026-10/orcd-memcheck-20261031.md).
# The build identity comes from EFS_GIT_ID, EFS_GIT_DESCRIBE (required; the
# release's own) and EFS_GIT_BRANCH (default devel).
# O8_STEPS (default "tsan asan leak") selects steps; EFS_SAN_ONLY narrows the sanitizer entries.
# Per step: log, exit status and seconds in OUTDIR/summary.txt. OUTDIR must not exist.
set -u
REL=$1; O=$2
[ -e "$O" ] && { echo "$O exists" >&2; exit 2; }
mkdir -p "$O"
T=/tmp/bq-o8-$$; mkdir -p "$T"
cp -a "$REL" "$T/src" && make -C "$T/src" clean > /dev/null 2>&1
export EFS_GIT_ID=${EFS_GIT_ID:?release build id} EFS_GIT_DESCRIBE=${EFS_GIT_DESCRIBE:?release describe}
export EFS_GIT_BRANCH=${EFS_GIT_BRANCH:-devel}
export EFS_RDMA_DEV=mlx5_0 EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_TRANSPORT=auto
export PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
export TMPDIR=$T/tmp ASAN_OPTIONS=detect_leaks=0; mkdir -p "$TMPDIR"
V=${EFS_VG_PREFIX:-$HOME/.local/valgrind-3.28.0}
step() { # name target [env...]
  n=$1; shift; t0=$(date +%s)
  (cd "$T/src" && "$@") > "$O/$n.log" 2>&1; rc=$?
  echo "$n rc=$rc seconds=$(( $(date +%s) - t0 ))" >> "$O/summary.txt"
}
case " ${O8_STEPS:-tsan asan leak} " in *' tsan '*) step tsan-check make tsan-check;; esac
case " ${O8_STEPS:-tsan asan leak} " in *' asan '*) step asan-check make asan-check;; esac
case " ${O8_STEPS:-tsan asan leak} " in *' leak '*)
CPATH=$V/include PATH=$V/bin:$PATH \
  step leak-check make leak-check EFS_LEAK_LOGDIR="$O/memcheck";; esac
echo "done $(date -u +%FT%TZ)" >> "$O/summary.txt"
"$HOME/bin/edelete" --force --delete "$T" > /dev/null 2>&1
