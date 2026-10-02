#!/bin/bash
# truncate_big.sh — W43 step c: truncate / O_TRUNC of a file whose lanes
# hold more than 32 chunks each.
#
# Oct 1 2026 22:00Z review: `apply truncate rc=-2` (NOMEM at the 33rd
# chunk DEL of a lane, TRUNC_IT_CAP) was answered OK, so `dd ... of=big`
# (O_TRUNC) onto an existing 1 GiB file left the old size and the old
# chunks and the client heard success. W43 step b makes the SETATTR fail
# (EIO) instead of lying; D25 (ask) decides how a big truncate reaches the
# KV. This script is the gate for both states:
#   - today (step b, no D25): every case must report TRUNC_ERR — the
#     truncate RETURNS AN ERROR and the file is unchanged. A case that
#     reports OK with the old size, or OK with the old md5, is the lie.
#   - after D25: every case must report PASS (size, md5, and the dropped
#     chunk row gone).
# The overall exit is 0 only when every case is PASS; TRUNC_ERR exits 3
# (expected until D25), a lie exits 1.
#
# Cases (each on a fresh file, non-zero source, flush in the clock):
#   1g-same   1 GiB file, dd 10 MiB of the SAME bytes with O_TRUNC
#   1g-diff   1 GiB file, dd 10 MiB of DIFFERENT bytes with O_TRUNC
#   300m-diff 300 MiB file (every lane > 32 chunks), dd 10 MiB different
#   1g-zero   1 GiB file, `truncate -s 0`
# Checks: stat size == 10 MiB (or 0); md5sum == md5 of the 10 MiB source
# (read after a drop of the client's cache via a fresh open with
# POSIX_FADV_DONTNEED is not enough on FUSE — we read through `dd
# iflag=direct`); `efs-mgmt raft-getchunks <leader> <ino> 100` (chunk 100
# = byte 12.5 MiB, past the new EOF) prints no row.
#
# Usage (from node9901 via efs-bg.sh; FUSE mounted on HOST; efs-mgmt
# built in /tmp/efs on the leader host):
#   tests/stress/truncate_big.sh
#   HOST=fcstor008.ib LEADER=172.16.223.57:19810 tests/stress/truncate_big.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
HOST="${HOST:-fcstor007.ib}"
MGMT_HOST="${MGMT_HOST:-fcstor003.ib}"
LEADER="${LEADER:-172.16.223.57:19810}"
MNT="${MNT:-/tmp/efs-mount}"
STAMP=$(date -u +%Y%m%d-%H%M%S)
DIR="$MNT/truncbig-$STAMP"
OUTDIR="${OUTDIR:-$HOME/git/efs/results/stress/truncate-big-$STAMP}"
mkdir -p "$OUTDIR"
say() { echo "[truncate-big] $*"; }
r() { EFS_SSH_TIMEOUT="${1}" "$SSH" "$HOST" "${2}"; }

r 10 "findmnt -no FSTYPE $MNT; stat $MNT/ >/dev/null && echo MOUNT_OK" 2>/dev/null \
    | grep -q MOUNT_OK || { say "FAIL: $HOST mount not usable"; exit 2; }
r 10 "mkdir $DIR && echo DIR_OK" | grep -q DIR_OK || { say "FAIL: mkdir $DIR"; exit 2; }

# Two distinct non-zero sources (local tmpfs on the client): A for the
# big file, B for the "different" rewrite.
r 60 "mkdir -p /dev/shm/truncbig && head -c $((1024*1024*1024)) /dev/urandom > /dev/shm/truncbig/A && head -c $((10*1024*1024)) /dev/urandom > /dev/shm/truncbig/B && md5sum /dev/shm/truncbig/B | cut -d' ' -f1 > /dev/shm/truncbig/B.md5 && head -c $((10*1024*1024)) /dev/shm/truncbig/A | md5sum | cut -d' ' -f1 > /dev/shm/truncbig/A10.md5 && echo SRC_OK" \
    | grep -q SRC_OK || { say "FAIL: source files"; exit 2; }

lies=0; errs=0; pass=0
# case <name> <big-bytes> <rewrite: same|diff|zero>
run_case() {
    local name=$1 big=$2 mode=$3 f="$DIR/$1" want_size want_md5 out rc
    say "case $name: big=$big mode=$mode"
    r 120 "dd if=/dev/shm/truncbig/A of=$f bs=1M count=$((big / 1048576)) conv=fsync status=none && stat -c %s $f" \
        > "$OUTDIR/$name.create" 2>&1
    if [ "$(tail -1 "$OUTDIR/$name.create")" != "$big" ]; then
        say "FAIL: $name create (size $(tail -1 "$OUTDIR/$name.create"), want $big)"; lies=$((lies + 1)); return
    fi
    local ino; ino=$(r 10 "stat -c %i $f")
    case $mode in
    same) want_size=$((10*1024*1024)); want_md5=$(r 10 "cat /dev/shm/truncbig/A10.md5")
          out=$(r 120 "dd if=/dev/shm/truncbig/A of=$f bs=1M count=10 conv=fsync status=none 2>&1; echo rc=\$?") ;;
    diff) want_size=$((10*1024*1024)); want_md5=$(r 10 "cat /dev/shm/truncbig/B.md5")
          out=$(r 120 "dd if=/dev/shm/truncbig/B of=$f bs=1M count=10 conv=fsync status=none 2>&1; echo rc=\$?") ;;
    zero) want_size=0; want_md5=$(r 10 "printf '' | md5sum | cut -d' ' -f1")
          out=$(r 60 "truncate -s 0 $f 2>&1; echo rc=\$?") ;;
    esac
    echo "$out" > "$OUTDIR/$name.truncate"
    rc=$(echo "$out" | sed -n 's/^rc=//p' | tail -1)
    local size md5 row
    size=$(r 10 "stat -c %s $f")
    md5=$(r 120 "dd if=$f bs=1M iflag=direct status=none | md5sum | cut -d' ' -f1")
    row=$(EFS_SSH_TIMEOUT=15 "$SSH" "$MGMT_HOST" "cd /tmp/efs && EFS_MGMT_CHUNKS=1 ./efs-mgmt raft-getchunks $LEADER $ino 100 2>&1" | tee "$OUTDIR/$name.chunk100")
    echo "name=$name ino=$ino rc=$rc size=$size want_size=$want_size md5=$md5 want_md5=$want_md5" | tee -a "$OUTDIR/summary.txt"
    if [ "$rc" != "0" ]; then
        if [ "$size" = "$big" ]; then
            say "TRUNC_ERR $name: truncate failed rc=$rc and the file is unchanged (expected until D25)"
            errs=$((errs + 1))
        else
            say "LIE $name: truncate failed rc=$rc but size moved to $size"
            lies=$((lies + 1))
        fi
        return
    fi
    if [ "$size" != "$want_size" ] || [ "$md5" != "$want_md5" ]; then
        say "LIE $name: truncate returned 0, size=$size want=$want_size md5 match=$([ "$md5" = "$want_md5" ] && echo yes || echo no)"
        lies=$((lies + 1)); return
    fi
    # raft-getchunks lists rows from chunk 100 on; any `ci=` line is a
    # chunk past the new EOF that the truncate left in the KV.
    if echo "$row" | grep -q '^  ci='; then
        say "LIE $name: chunk 100 still has a row after the truncate: $(echo "$row" | head -1)"
        lies=$((lies + 1)); return
    fi
    say "PASS $name"
    pass=$((pass + 1))
}

run_case 1g-same  $((1024*1024*1024)) same
run_case 1g-diff  $((1024*1024*1024)) diff
run_case 300m-diff $((300*1024*1024)) diff
run_case 1g-zero  $((1024*1024*1024)) zero

r 60 "rm -rf /dev/shm/truncbig; rm -rf $DIR" >/dev/null 2>&1
say "pass=$pass trunc_err=$errs lies=$lies -> $OUTDIR"
if [ "$lies" -gt 0 ]; then exit 1; fi
if [ "$errs" -gt 0 ]; then exit 3; fi
exit 0
