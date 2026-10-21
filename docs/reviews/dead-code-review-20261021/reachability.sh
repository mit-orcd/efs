#!/bin/bash
# reachability.sh TREE OUT [MAKE_VARIABLES...]
#
# Whole-program reachability scan used by docs/reviews/dead-code-review-20261021.md.
# Builds TREE at -O0 -fno-inline with one section per function, then links
# every production binary against the WHOLE library with --gc-sections and
# records which function sections the linker discards. A function discarded
# from all five production links is unreachable in THIS build configuration.
# The test binaries are relinked the same way and their surviving symbols
# recorded, so callers can separate "dead" from "reachable only from tests".
# Extra arguments are passed to make to scan a build variant, for example
# EFS_FAULTS=1 or EFS_MEMCHECK=1; a result holds only for the variants scanned.
#
# WRITES TO BOTH LOCATIONS: it runs `make clean`, rebuilds every object and
# test binary, and deletes and relinks test binaries inside TREE; its own
# links and listings go under OUT. TREE must therefore be a disposable
# `git archive` extraction of the reviewed commit. The script refuses a git
# checkout, a deployed release or src tree, and a tree without the marker
# file TREE/.efs-audit-tree (create it yourself after extracting).
#
# Linux + gcc + fuse3 (pkg-config) + libibverbs. Feed OUT to classify.py,
# run from a git checkout of the same commit.
set -eu
TREE=$(cd "$1" && pwd); OUT=$2; shift 2
case "$TREE" in /data1/efs/releases/*|/data1/efs/src|/data1/efs/src/*)
    echo "refusing deployed tree $TREE" >&2; exit 2;; esac
[ -e "$TREE/.git" ] && { echo "refusing git checkout $TREE; use a git archive copy" >&2; exit 2; }
[ -f "$TREE/.efs-audit-tree" ] || { echo "refusing $TREE: no .efs-audit-tree marker" >&2; exit 2; }
mkdir -p "$OUT"
cd "$TREE"
FLAGS="-O0 -fno-inline -ffunction-sections -fdata-sections"
LD="-lpthread -lm -ldl -libverbs"
GC="-Wl,--gc-sections -Wl,--print-gc-sections"
WA="-Wl,--whole-archive libefs.a -Wl,--no-whole-archive"
make clean >/dev/null
make -j"$(nproc)" EFS_GIT_ID=audit EXTRA_DEFS="$FLAGS" "$@" all >"$OUT/build.log" 2>&1
SERVER=$(make -pn 2>/dev/null | sed -n 's/^SERVER_SRCS = //p' | head -1 | sed 's/\.c/.o/g')
BENCH="src/client/efs_bench.o src/bench/bench_local.o src/bench/blake3_bench.o
       src/bench/io_bench.o src/bench/storage_principles.o src/bench/store.o
       src/bench/store_nvme.o src/bench/storage_roots.o src/bench/writer.o
       src/bench/iostats.o src/bench/thread.o src/bench/reclaim.o"
gcc -O0 -o "$OUT/efsd"  $SERVER                $WA $LD $GC 2>"$OUT/efsd.gc"
gcc -O0 -o "$OUT/fuse"  src/client/efs_fuse.o  $WA $LD $(pkg-config --libs fuse3) $GC 2>"$OUT/fuse.gc"
gcc -O0 -o "$OUT/bench" $BENCH                 $WA $LD $GC 2>"$OUT/bench.gc"
gcc -O0 -o "$OUT/mgmt"  src/mgmt/efs_mgmt.o    $WA $LD $GC 2>"$OUT/mgmt.gc"
gcc -O0 -o "$OUT/query" src/query/efs_query.o  $WA $LD $GC 2>"$OUT/query.gc"

# Test binaries: ordinary links (an unpulled object is unused by that test),
# with --gc-sections, then every surviving defined function with its source.
TB=$(make -pn 2>/dev/null | sed -n 's/^TEST_BINS = //p' | head -1)
EXTRA="tests/test_put_storage_fence tests/test_put_upload_server
       tests/test_put_upload_client tests/test_data_commit tests/faults/fault_sim
       tests/measure/metadata_route_smoke tests/measure/getattr_lane_smoke
       tests/measure/lsm_index_smoke tests/measure/kv_block_hash_smoke"
rm -f $TB $EXTRA
make -j"$(nproc)" EFS_GIT_ID=audit EXTRA_DEFS="$FLAGS" "$@" LDFLAGS="$LD -Wl,--gc-sections" \
    tests $EXTRA >"$OUT/tests.log" 2>&1
for b in $TB $EXTRA; do
    nm -l --defined-only "$b" 2>/dev/null | awk '$2~/[tTwW]/{print $3, $4}'
done | sed "s|$PWD/||" | sort -u >"$OUT/testsyms.txt"
