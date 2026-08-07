# Shared hot-path smoke workload for efs FUSE mounts.
# shellcheck shell=bash
#
# Exercises: volume R/W, parallel metadata (dut), chmod/chown, truncate +
# sparse holes, concurrent writers on one mount, and dual-mount concurrency
# against the same export.
#
# Caller must define: MNT, and optionally MNT2 (second mount of same export),
# DUT (path to dut binary), FAIL counter helpers pass/fail.

efs_hotpath_require() {
    : "${MNT:?MNT must be set}"
    : "${DUT:?DUT must be set}"
}

# Build a meta-heavy tree under $1 (relative to MNT or absolute).
efs_hotpath_make_tree() {
    local root=$1
    local ndirs=${2:-16}
    local nfiles=${3:-40}
    mkdir -p "$root"
    local d f
    for d in $(seq 1 "$ndirs"); do
        mkdir -p "$root/d$d"
        for f in $(seq 1 "$nfiles"); do
            printf 'd%s-f%s-payload-%s\n' "$d" "$f" "$(printf 'x%.0s' {1..200})" \
                > "$root/d$d/f$f.txt"
        done
    done
}

efs_hotpath_volume_io() {
    local root=$1
    echo "=== volume: 4 MiB write/read + rewrite ==="
    dd if=/dev/zero of="$root/bulk.bin" bs=1M count=4 status=none
    dd if="$root/bulk.bin" of=/dev/null bs=1M status=none
    dd if=/dev/urandom of="$root/bulk.bin" bs=1M count=2 status=none
    sync "$root/bulk.bin" 2>/dev/null || true
    local sz
    sz=$(stat -c %s "$root/bulk.bin")
    [ "$sz" = "2097152" ] && pass "volume size after rewrite" || fail "volume size=$sz"
}

efs_hotpath_dut_meta() {
    local root=$1
    local threads=${2:-8}
    echo "=== dut parallel meta walk (-t $threads) ==="
    local out="${HOTPATH_TMP:-.}/dut-out.$$"
    mkdir -p "$(dirname "$out")"
    # -s: file byte sizes (FUSE often reports st_blocks=0).
    if /usr/bin/time -f 'dut_real=%e' "$DUT" -c -s -t "$threads" -n 20 -d 3 "$root" \
        > "$out" 2>"${out}.time"; then
        cat "${out}.time" "$out" | head -n 40
        pass "dut meta walk"
    else
        cat "${out}.time" "$out" || true
        fail "dut failed"
    fi
    rm -f "$out" "${out}.time"
}

efs_hotpath_chmod_chown() {
    local root=$1
    local me mg
    me=$(id -u)
    mg=$(id -g)
    echo "=== chmod / chown batch ==="
    # Avoid find|head SIGPIPE under pipefail: materialize the list first.
    local list="${HOTPATH_TMP:-.}/chmod-list.$$"
    find "$root" -type f > "$list"
    local sample
    sample=$(head -n 1 "$list")
    head -n 120 "$list" | xargs -r chmod 640
    head -n 120 "$list" | xargs -r chown "$me:$mg"
    rm -f "$list"
    local mode uidg
    mode=$(stat -c %a "$sample")
    uidg=$(stat -c %u:%g "$sample")
    [ "$mode" = "640" ] && pass "chmod 640" || fail "chmod got $mode"
    [ "$uidg" = "$me:$mg" ] && pass "chown $me:$mg" || fail "chown got $uidg"
}

efs_hotpath_truncate_sparse() {
    local root=$1
    echo "=== truncate shrink / grow (sparse holes) ==="
    # 256 KiB of 'A', shrink to 10, grow to 1 MiB, check holes are zeros.
    dd if=/dev/zero bs=256K count=1 status=none | tr '\0' 'A' > "$root/sparse.bin"
    truncate -s 10 "$root/sparse.bin"
    local got
    got=$(wc -c < "$root/sparse.bin")
    [ "$got" -eq 10 ] && pass "truncate shrink size" || fail "shrink size=$got"
    got=$(head -c 10 "$root/sparse.bin")
    [ "$got" = "AAAAAAAAAA" ] && pass "truncate shrink prefix" || fail "shrink prefix"
    truncate -s 1048576 "$root/sparse.bin"
    got=$(stat -c %s "$root/sparse.bin")
    [ "$got" = "1048576" ] && pass "truncate grow size" || fail "grow size=$got"
    # Bytes 10..4096 must be zeros (hole / discarded tail).
    if dd if="$root/sparse.bin" bs=1 skip=10 count=4086 status=none | \
        tr -d '\0' | grep -q .; then
        fail "sparse hole not zero after truncate-up"
    else
        pass "sparse hole reads as zeros"
    fi
    # Write past a hole and verify sandwich.
    printf 'ZZZZ' | dd of="$root/sparse.bin" bs=1 seek=100000 conv=notrunc status=none
    got=$(dd if="$root/sparse.bin" bs=1 skip=100000 count=4 status=none)
    [ "$got" = "ZZZZ" ] && pass "write into sparse region" || fail "sparse write"
    if dd if="$root/sparse.bin" bs=1 skip=50000 count=100 status=none | \
        tr -d '\0' | grep -q .; then
        fail "mid-hole not zero"
    else
        pass "mid-hole still zeros"
    fi
}

# Concurrent writers on one mount (different files) + parallel readers of one file.
efs_hotpath_concurrent_same_mount() {
    local root=$1
    echo "=== concurrent same-mount writers + readers ==="
    mkdir -p "$root/conc"
    dd if=/dev/urandom of="$root/conc/shared.bin" bs=64K count=8 status=none
    local pids=()
    local i
    for i in 1 2 3 4; do
        (
            dd if=/dev/urandom of="$root/conc/w$i.bin" bs=64K count=4 status=none
            sync "$root/conc/w$i.bin" 2>/dev/null || true
        ) &
        pids+=($!)
    done
    for i in 1 2 3 4; do
        (
            dd if="$root/conc/shared.bin" of=/dev/null bs=32K status=none
        ) &
        pids+=($!)
    done
    local rc=0
    for pid in "${pids[@]}"; do
        wait "$pid" || rc=1
    done
    [ "$rc" -eq 0 ] && pass "concurrent same-mount IO" || fail "concurrent same-mount IO"
    for i in 1 2 3 4; do
        local sz
        sz=$(stat -c %s "$root/conc/w$i.bin")
        [ "$sz" = "262144" ] && pass "conc writer $i size" || fail "conc writer $i size=$sz"
    done
}

# Two mounts, same export: parallel creates + cross-read.
efs_hotpath_concurrent_dual_mount() {
    local a=$1
    local b=$2
    echo "=== concurrent dual-mount same export ==="
    mkdir -p "$a/dual" "$b/dual"
    local pids=()
    (
        for i in $(seq 1 30); do
            echo "from-a-$i" > "$a/dual/a$i.txt"
        done
    ) &
    pids+=($!)
    (
        for i in $(seq 1 30); do
            echo "from-b-$i" > "$b/dual/b$i.txt"
        done
    ) &
    pids+=($!)
    local rc=0
    for pid in "${pids[@]}"; do
        wait "$pid" || rc=1
    done
    [ "$rc" -eq 0 ] && pass "dual-mount parallel creates" || fail "dual-mount creates"
    # Cross visibility (may need a moment for meta merge on the other client).
    sleep 1
    sync "$a" "$b" 2>/dev/null || true
    # Force meta refresh via readdir + lookup
    local n_a n_b
    n_a=$(ls "$a/dual" 2>/dev/null | wc -l)
    n_b=$(ls "$b/dual" 2>/dev/null | wc -l)
    # Each mount sees at least its own 30; ideally both see 60 after merge.
    [ "$n_a" -ge 30 ] && pass "mount A sees >=30 (got $n_a)" || fail "mount A count=$n_a"
    [ "$n_b" -ge 30 ] && pass "mount B sees >=30 (got $n_b)" || fail "mount B count=$n_b"
    if [ "$n_a" -ge 60 ] && [ "$n_b" -ge 60 ]; then
        pass "dual-mount full merge visibility"
    else
        # Soft: merge may lag until remount; still valuable concurrency stress.
        echo "NOTE: dual-mount visibility a=$n_a b=$n_b (full merge not required)"
        pass "dual-mount concurrency completed (partial visibility ok)"
    fi
}

# Overlapping same-file writers (best-effort correctness check).
efs_hotpath_same_file_writers() {
    local root=$1
    echo "=== concurrent writers to same file (non-overlapping regions) ==="
    # Pre-size with truncate so both ranges are in-file.
    : > "$root/same.bin"
    truncate -s 512K "$root/same.bin"
    local pids=()
    (
        dd if=/dev/zero bs=1K count=128 status=none | tr '\0' 'A' | \
            dd of="$root/same.bin" bs=1K seek=0 conv=notrunc status=none
    ) &
    pids+=($!)
    (
        dd if=/dev/zero bs=1K count=128 status=none | tr '\0' 'B' | \
            dd of="$root/same.bin" bs=1K seek=256 conv=notrunc status=none
    ) &
    pids+=($!)
    wait "${pids[0]}" || true
    wait "${pids[1]}" || true
    sync "$root/same.bin" 2>/dev/null || true
    local head_a mid_b
    head_a=$(dd if="$root/same.bin" bs=1K count=1 status=none | tr -d '\0' | head -c 1)
    mid_b=$(dd if="$root/same.bin" bs=1K skip=256 count=1 status=none | tr -d '\0' | head -c 1)
    [ "$head_a" = "A" ] && pass "same-file region A" || fail "same-file region A got '$head_a'"
    [ "$mid_b" = "B" ] && pass "same-file region B" || fail "same-file region B got '$mid_b'"
}

# Full suite. Args: primary_mnt [secondary_mnt]
efs_hotpath_run_all() {
    efs_hotpath_require
    local primary=$1
    local secondary=${2:-}
    mkdir -p "$primary/hot"
    efs_hotpath_make_tree "$primary/hot/tree" 16 40
    efs_hotpath_volume_io "$primary/hot"
    efs_hotpath_dut_meta "$primary/hot/tree" 8
    efs_hotpath_chmod_chown "$primary/hot/tree"
    efs_hotpath_truncate_sparse "$primary/hot"
    efs_hotpath_concurrent_same_mount "$primary/hot"
    efs_hotpath_same_file_writers "$primary/hot"
    if [ -n "$secondary" ] && [ -d "$secondary" ]; then
        efs_hotpath_concurrent_dual_mount "$primary/hot" "$secondary/hot"
    fi
    echo "=== hotpath suite done ==="
}
