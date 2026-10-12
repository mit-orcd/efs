#!/bin/bash
# Build a private upstream verbs runtime with real Memcheck annotations.
# Usage: build_valgrind_rdma.sh NEW_WORKDIR RDMA_CORE_TAR_GZ VALGRIND_TAR_BZ2
set -euo pipefail
test "$#" = 3 || { echo "$0 NEW_WORKDIR RDMA_CORE_TAR_GZ VALGRIND_TAR_BZ2" >&2; exit 2; }
test "$(id -u)" != 0 || { echo 'run as the non-root EFS account' >&2; exit 2; }
work=$(realpath -m "$1")
rdma=$(realpath "$2")
vg=$(realpath "$3")
test ! -e "$work" || { echo 'work directory must be new' >&2; exit 2; }
# Pinned official upstream source archives; never install into system paths.
echo "4dca3dd4eed6e5e1fd21ec32244a753b95109c8a845ebda0e58dac53528808aa  $rdma" | sha256sum -c -
echo "8d54c717029106f1644aadaf802ab9692e53d93dd015cbd19e74190eba616bd7  $vg" | sha256sum -c -
mkdir -p "$work"
cd "$work"
tar -xzf "$rdma"
tar -xjf "$vg" valgrind-3.26.0/include/valgrind.h valgrind-3.26.0/memcheck/memcheck.h valgrind-3.26.0/drd/drd.h
mkdir -p vg-headers/valgrind
cp valgrind-3.26.0/include/valgrind.h valgrind-3.26.0/memcheck/memcheck.h valgrind-3.26.0/drd/drd.h vg-headers/valgrind/
cmake -S rdma-core-59.0 -B rdma-core-build -DIN_PLACE=1 \
    -DENABLE_VALGRIND=ON -DNO_MAN_PAGES=ON -DENABLE_RESOLVE_NEIGH=OFF \
    -DENABLE_STATIC=OFF "-DCMAKE_C_FLAGS=-I$work/vg-headers" > configure.log 2>&1
grep -qx 'HAVE_VALGRIND_MEMCHECK:INTERNAL=1' rdma-core-build/CMakeCache.txt
grep -qx 'HAVE_VALGRIND_DRD:INTERNAL=1' rdma-core-build/CMakeCache.txt
cmake --build rdma-core-build --target ibverbs mlx5 -j4 > build.log 2>&1
echo "Use EFS_VG_RDMA_RUNTIME=$work with tests/valgrind_leaks.sh in an isolated EFS checkout."
