#!/bin/bash
# reachability.sh TREE OUT
#
# Whole-program reachability scan used by docs/reviews/dead-code-review-20261021.md.
# Builds TREE (a clean `git archive` of the reviewed commit, never a deployed
# release) at -O0 -fno-inline with one section per function, then links every
# production binary against the WHOLE library with --gc-sections and records
# which function sections the linker discards. A function discarded from all
# five production links is unreachable in production. The test binaries are
# relinked the same way and their surviving symbols recorded, so callers can
# separate "dead everywhere" from "reachable only from tests".
#
# Linux + gcc + fuse3 (pkg-config) + libibverbs. Writes only under OUT.
# Prints nothing useful itself; feed OUT to classify.py.
set -eu
TREE=$1; OUT=$2
mkdir -p "$OUT"
cd "$TREE"
FLAGS="-O0 -fno-inline -ffunction-sections -fdata-sections"
LD="-lpthread -lm -ldl -libverbs"
GC="-Wl,--gc-sections -Wl,--print-gc-sections"
WA="-Wl,--whole-archive libefs.a -Wl,--no-whole-archive"
make clean >/dev/null
make -j"$(nproc)" EFS_GIT_ID=audit EXTRA_DEFS="$FLAGS" all >"$OUT/build.log" 2>&1
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
make -j"$(nproc)" EFS_GIT_ID=audit EXTRA_DEFS="$FLAGS" LDFLAGS="$LD -Wl,--gc-sections" \
    tests $EXTRA >"$OUT/tests.log" 2>&1
for b in $TB $EXTRA; do
    nm -l --defined-only "$b" 2>/dev/null | awk '$2~/[tTwW]/{print $3, $4}'
done | sed "s|$PWD/||" | sort -u >"$OUT/testsyms.txt"
