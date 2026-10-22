#!/bin/bash
# Packed-segment validation cost, devel API vs the validate-once view API.
# Times efs_pack_ledger_init and a compaction-style enumeration of every
# record of one full (512-record) segment, for two git revisions:
#   scripts/pack_validate_bench.sh [OLD_REV [NEW_REV]]
# Defaults: 50bbd693a (pack code as profiled on build 08cc16af4f77) and HEAD.
# OLD_REV must predate efs_pack_view_open (uses efs_pack_record_count/at);
# NEW_REV must have it. Builds only packed_segment.c, pack_ledger.c,
# checksum.c, erasure.c and portable BLAKE3, so it runs on Linux and macOS.
# Output: one line per revision; nothing is written outside a temp dir.
set -euo pipefail
OLD=${1:-50bbd693a}; NEW=${2:-HEAD}; ITER=${ITER:-20}
ROOT=$(git rev-parse --show-toplevel)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/bench.c" <<'C'
#include "efs/pack_ledger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv)
{
    static struct efs_pack_record r[EFS_PACK_MAX_RECORDS];
    static struct efs_pack_ref refs[EFS_PACK_MAX_RECORDS];
    static uint8_t pay[EFS_PACK_MAX_RECORDS][150];
    static struct efs_pack_ledger l;
    int it = argc > 1 ? atoi(argv[1]) : 20;
    uint8_t *img = malloc(EFS_SEGMENT_MAX_SIZE);
    for (unsigned i = 0; i < EFS_PACK_MAX_RECORDS; i++) {
        memset(pay[i], (int)i, sizeof(pay[i]));
        r[i] = (struct efs_pack_record){.ino = 10 + i, .inode_generation = 1, .record_generation = 1 + i,
                                        .len = sizeof(pay[i]), .data = pay[i]};
    }
    size_t g;
    struct efs_opid o = {.client_uuid = {1}, .session_epoch = 1, .seq = 1};
    if (!img || efs_pack_build(1, 1, 0, r, EFS_PACK_MAX_RECORDS, img, EFS_SEGMENT_MAX_SIZE, &g, refs)) return 1;
    double t = now();
    for (int i = 0; i < it; i++)
        if (efs_pack_ledger_init(img, g, refs, EFS_PACK_MAX_RECORDS, &o, 7, 0, &l)) return 2;
    double init = (now() - t) / it * 1e3;
    t = now();
    for (int i = 0; i < it; i++) {
        struct efs_pack_ref x;
#ifdef OLD_API
        uint32_t n;
        if (efs_pack_record_count(img, g, &n)) return 3;
        for (uint32_t k = 0; k < n; k++) if (efs_pack_record_at(img, g, k, &x)) return 3;
#else
        struct efs_pack_view v;
        if (efs_pack_view_open(img, g, &v)) return 3;
        for (uint32_t k = 0; k < v.count; k++) if (efs_pack_view_record_at(&v, k, &x)) return 3;
#endif
    }
    printf("records=%u geometry=%zu ledger_init_ms=%.3f enumerate_ms=%.3f\n",
           EFS_PACK_MAX_RECORDS, g, init, (now() - t) / it * 1e3);
    return 0;
}
C
build() { # rev dir extra-cflags
    mkdir -p "$2"
    (cd "$ROOT" && git archive "$1" include src/common/packed_segment.c src/common/pack_ledger.c \
        src/common/checksum.c src/data/erasure.c deps/blake3) | tar -x -C "$2"
    cc -O3 -std=c99 -D_GNU_SOURCE -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
       -DBLAKE3_USE_NEON=0 $3 -I"$2/include" -I"$2/src/common" -I"$2/deps/blake3" -o "$2/bench" "$T/bench.c" \
       "$2/src/common/packed_segment.c" "$2/src/common/pack_ledger.c" "$2/src/common/checksum.c" \
       "$2/src/data/erasure.c" "$2/deps/blake3/blake3.c" "$2/deps/blake3/blake3_dispatch.c" \
       "$2/deps/blake3/blake3_portable.c" -lpthread -lm
}
build "$OLD" "$T/old" -DOLD_API
build "$NEW" "$T/new" ""
echo "old $(git -C "$ROOT" rev-parse --short "$OLD") $("$T/old/bench" "$ITER")"
echo "new $(git -C "$ROOT" rev-parse --short "$NEW") $("$T/new/bench" "$ITER")"
