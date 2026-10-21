#!/bin/bash
# reachability.sh TREE OUT [VARIANT...]
#
# Whole-program reachability scan used by docs/reviews/dead-code-review-20261021.md.
# Builds TREE at -O0 -fno-inline with one section per function, then links
# every production binary against the WHOLE library with --gc-sections and
# records which function sections the linker discards. A function discarded
# from all five production links is unreachable in THIS build configuration.
# The test binaries are relinked the same way and their surviving symbols
# recorded, so classify.py can separate "dead" from "reachable only from tests".
#
# VARIANT is restricted to the Makefile feature toggles EFS_FAULTS=1 and
# EFS_MEMCHECK=1. They add defines only; they change neither the source lists
# nor the compiler or link libraries hardcoded below. A result holds only for
# the variants actually scanned.
#
# WRITES TO BOTH LOCATIONS: it runs `make clean`, rebuilds every object and
# test binary, and deletes and relinks test binaries inside TREE; its own
# links and listings go under OUT. TREE must be a disposable `git archive`
# extraction of the reviewed commit. The marker file TREE/.efs-audit-tree is
# the caller's assertion that it is. The other checks only catch common
# accidents (a Git working tree, the usual EFS deploy paths); they are not
# deployment detection and not a security boundary.
#
# Linux + gcc + fuse3 (pkg-config) + libibverbs. Exits nonzero, and writes
# OUT/INCOMPLETE, if any link or symbol extraction fails. Feed a complete OUT
# to classify.py, run from a git checkout of the same commit.
set -euo pipefail
die() { echo "reachability: $*" >&2; [ -n "${OUT:-}" ] && [ -d "$OUT" ] && echo "$*" >"$OUT/INCOMPLETE"; exit 2; }
[ $# -ge 2 ] || die "usage: reachability.sh TREE OUT [EFS_FAULTS=1|EFS_MEMCHECK=1]..."
TREE=$(cd "$1" 2>/dev/null && pwd -P) || die "no such TREE: $1"
mkdir -p "$2" && OUT=$(cd "$2" && pwd -P) || die "cannot create OUT: $2"
shift 2
for v in "$@"; do
    case "$v" in EFS_FAULTS=1|EFS_MEMCHECK=1) ;; *) die "unsupported variant: $v";; esac
done
case "$TREE" in /data1/efs/releases/*|/data1/efs/src|/data1/efs/src/*|/data1/efs/orcd-mini-client/*)
    die "refusing a usual EFS deploy path: $TREE";; esac
if git -C "$TREE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    die "refusing $TREE: inside a Git working tree; use a git archive copy"
fi
[ -f "$TREE/.efs-audit-tree" ] || die "refusing $TREE: no .efs-audit-tree marker"
case "$OUT/" in "$TREE"/*) die "OUT must be outside TREE";; esac
rm -f "$OUT/INCOMPLETE"
cd "$TREE"

# Read the Makefile's lists once, with the same variables as the build. No
# pipeline ends early, so pipefail cannot trip on SIGPIPE.
make -pn EFS_GIT_ID=audit "$@" >"$OUT/make-db.txt" 2>/dev/null || true
makevar() { awk -v k="$1 = " 'index($0,k)==1 && !seen {print substr($0,length(k)+1); seen=1}' "$OUT/make-db.txt"; }
SERVER=$(makevar SERVER_SRCS | sed 's/\.c/.o/g')
TB=$(makevar TEST_BINS)
[ -n "$SERVER" ] && [ -n "$TB" ] || die "could not read SERVER_SRCS/TEST_BINS from the Makefile"

FLAGS="-O0 -fno-inline -ffunction-sections -fdata-sections"
LD="-lpthread -lm -ldl -libverbs"
GC="-Wl,--gc-sections -Wl,--print-gc-sections"
WA="-Wl,--whole-archive libefs.a -Wl,--no-whole-archive"
make clean >/dev/null
make -j"$(nproc)" EFS_GIT_ID=audit EXTRA_DEFS="$FLAGS" "$@" all >"$OUT/build.log" 2>&1 ||
    die "build failed; see $OUT/build.log"
BENCH="src/client/efs_bench.o src/bench/bench_local.o src/bench/blake3_bench.o
       src/bench/io_bench.o src/bench/storage_principles.o src/bench/store.o
       src/bench/store_nvme.o src/bench/storage_roots.o src/bench/writer.o
       src/bench/iostats.o src/bench/thread.o src/bench/reclaim.o"
FUSE_LIBS=$(pkg-config --libs fuse3) || die "pkg-config fuse3 failed"
link() { local name=$1; shift; gcc -O0 -o "$OUT/$name" "$@" 2>"$OUT/$name.gc" || die "link $name failed; see $OUT/$name.gc"; }
link efsd  $SERVER               $WA $LD $GC
link fuse  src/client/efs_fuse.o $WA $LD $FUSE_LIBS $GC
link bench $BENCH                $WA $LD $GC
link mgmt  src/mgmt/efs_mgmt.o   $WA $LD $GC
link query src/query/efs_query.o $WA $LD $GC

# Test binaries: ordinary links (an unpulled object is unused by that test),
# with --gc-sections, then every surviving defined function with its source.
EXTRA="tests/test_put_storage_fence tests/test_put_upload_server
       tests/test_put_upload_client tests/test_data_commit tests/faults/fault_sim
       tests/measure/metadata_route_smoke tests/measure/getattr_lane_smoke
       tests/measure/lsm_index_smoke tests/measure/kv_block_hash_smoke"
rm -f $TB $EXTRA
make -j"$(nproc)" EFS_GIT_ID=audit EXTRA_DEFS="$FLAGS" "$@" LDFLAGS="$LD -Wl,--gc-sections" \
    tests $EXTRA >"$OUT/tests.log" 2>&1 || die "test build failed; see $OUT/tests.log"
: >"$OUT/testsyms.raw"
for b in $TB $EXTRA; do
    [ -x "$b" ] || die "expected test binary missing: $b"
    nm -l --defined-only "$b" >"$OUT/nm.tmp" 2>/dev/null || die "nm failed on $b"
    n=$(awk '$2~/[tTwW]/{print $3, $4}' "$OUT/nm.tmp" | tee -a "$OUT/testsyms.raw" | wc -l)
    [ "$n" -gt 0 ] || die "no function symbols extracted from $b"
done
rm -f "$OUT/nm.tmp"
sed "s|$TREE/||" "$OUT/testsyms.raw" | sort -u >"$OUT/testsyms.txt"
rm -f "$OUT/testsyms.raw"
echo "$*" >"$OUT/COMPLETE"
