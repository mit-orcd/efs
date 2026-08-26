#!/bin/bash
# A-side posix2 driver. Local --exec for side a; NFS mailbox for side b.
set -u
PY=${1:?posix_2client.py}
MNT=${2:?mount}
WORK=${3:?work dir}
OUT=${4:?results tsv}

mkdir -p "$(dirname "$OUT")" "$WORK"
npass=0
nfail=0
t0=$(date +%s)

run_a() {
    local name=$1 idx=$2
    python3 "$PY" --exec "$name" "$idx" "$MNT/posix-2c/$name" || true
}

run_b() {
    local name=$1 idx=$2
    local tok=${name}-${idx}-$$-$(date +%s%N)
    rm -f "$WORK/out"
    printf '%s %s %s %s\n' "$name" "$idx" "$MNT/posix-2c/$name" "$tok" > "$WORK/cmd.tmp"
    mv "$WORK/cmd.tmp" "$WORK/cmd"
    local i out
    for i in $(seq 1 1600); do
        ls "$WORK" >/dev/null 2>&1 || true
        if [ -f "$WORK/out" ]; then
            out=$(cat "$WORK/out")
            if printf '%s\n' "$out" | grep -q "TOKEN	$tok"; then
                printf '%s\n' "$out"
                return 0
            fi
        fi
        sleep 0.05
    done
    echo "RESULT	FAIL	B timeout waiting for poller tok=$tok"
}

do_test() {
    local name=$1
    shift
    local idx=0 out line status detail
    for side in "$@"; do
        if [ "$side" = a ]; then
            out=$(run_a "$name" "$idx")
        else
            out=$(run_b "$name" "$idx")
        fi
        line=$(printf '%s\n' "$out" | grep '^RESULT	' | tail -1 || true)
        if [ -z "$line" ]; then
            echo -e "$name\tFAIL\tno RESULT step $idx side=$side: ${out:0:180}" >> "$OUT"
            echo "FAIL $name  no RESULT step $idx side=$side"
            nfail=$((nfail + 1))
            return 0
        fi
        status=$(printf '%s\n' "$line" | cut -f2)
        detail=$(printf '%s\n' "$line" | cut -f3-)
        if [ "$status" != PASS ]; then
            echo -e "$name\tFAIL\t$detail" >> "$OUT"
            echo "FAIL $name  $detail"
            nfail=$((nfail + 1))
            return 0
        fi
        idx=$((idx + 1))
    done
    echo -e "$name\tPASS\t" >> "$OUT"
    echo "pass $name"
    npass=$((npass + 1))
}

echo "# posix-2client $(hostname -s) mnt=$MNT $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT"
echo -e "test\tresult\tdetail" >> "$OUT"

do_test peer_mkdir_visible a b
do_test peer_create_visible a b
do_test peer_write_read a b
do_test peer_listdir_siblings a b
do_test peer_shared_creat a b
do_test peer_open_existing a b
do_test peer_creat_excl_eexist a b
do_test peer_stat_size a b
do_test peer_rename_visible a b
do_test peer_mkdir_then_create a b
do_test peer_shared_pwrite a b a
do_test peer_unlink_gone a b
do_test peer_chmod_visible a b
do_test peer_utimens_visible a b
do_test peer_truncate_visible a b
do_test peer_hardlink_visible a b
do_test peer_symlink_visible a b
do_test peer_negative_dentry b a b
do_test peer_fsync_then_read a b
do_test peer_mkdir_rmdir_recreate a b
do_test peer_oappend a b a
do_test peer_rename_over a b
do_test peer_flock_exclusive a b a
do_test peer_unlink_while_b_has_fd a b a b
do_test peer_rmdir_gone a b
do_test peer_nlink_after_link a b
do_test peer_hardlink_write a b
do_test peer_rename_dir a b
do_test peer_o_trunc_visible a b
do_test peer_unlink_recreate a b
do_test peer_lstat_symlink_size a b
do_test peer_listdir_after_unlink a b
do_test peer_chmod_via_hardlink a b
do_test peer_sparse_size a b

t1=$(date +%s)
echo "# summary pass=$npass fail=$nfail skip=0 total=$((npass + nfail)) dur=$((t1 - t0))" >> "$OUT"
echo "PAIR_DONE pass=$npass fail=$nfail dur=$((t1 - t0))"
touch "$WORK/stop"
