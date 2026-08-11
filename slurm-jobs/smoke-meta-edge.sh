#!/bin/bash
#SBATCH --job-name=efs-smoke-metaedge
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-metaedge-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-metaedge-%j.err
#
# Edge cases that break metadata on one node (s2) and observe catch-up/heal:
#   A) wipe s2 meta-table fragments offline, restart → heal on rebuild
#   B) corrupt s2 meta fragments (checksum mismatch) → heal
#   C) delete s2 metadata.bin only (keep bad/empty root) → peer fetch/catchup
#   D) live wipe of s2 meta frags + new writes (PUT_META) → catchup heal
#   E) mid-flush abort (dual-slot) then wipe s2 + recover prior gen mountable

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-metaedge-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1994
P2=1995
P3=1996
MNT="$LOCAL/mnt"
FAIL=0
EXPORT="metaedge"
META_SHARD="0922/3372/0368/5477/5810"
REVIEW="$OUT/REVIEW.txt"

pass() { echo "PASS: $*"; echo "PASS: $*" >>"$REVIEW"; }
fail() { echo "FAIL: $*"; echo "FAIL: $*" >>"$REVIEW"; FAIL=1; }
note() { echo "NOTE: $*"; echo "NOTE: $*" >>"$REVIEW"; }

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

count_meta_frags() {
    local root=$1
    find "$root/data/exports" -type f ! -name '*.sum' 2>/dev/null \
        | grep -c "$META_SHARD" || true
}

wipe_meta_frags() {
    local root=$1
    find "$root/data/exports" -type f 2>/dev/null | grep "$META_SHARD" \
        | while read -r f; do rm -f "$f"; done
}

corrupt_meta_frags() {
    local root=$1
    find "$root/data/exports" -type f ! -name '*.sum' 2>/dev/null | grep "$META_SHARD" \
        | while read -r f; do
            dd if=/dev/urandom of="$f" bs=1024 count=4 conv=notrunc status=none 2>/dev/null || true
        done
}

wait_port() {
    local port=$1
    for i in $(seq 1 40); do
        if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

wait_log() {
    local file=$1
    local pat=$2
    local secs=${3:-30}
    for i in $(seq 1 "$secs"); do
        if grep -qE "$pat" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

start_s2() {
    : >"$OUT/s2.stdout"
    ./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --writers 4 \
        --join "$IP:$P1" >"$OUT/s2.stdout" 2>&1 &
    S2=$!
    wait_port "$P2" || { echo "s2 listen failed"; cat "$OUT/s2.stdout"; return 1; }
}

stop_s2() {
    if [ -n "${S2:-}" ]; then
        kill -KILL "$S2" 2>/dev/null || true
        wait "$S2" 2>/dev/null || true
        S2=
        sleep 0.5
    fi
}

mount_fuse() {
    local log=$1
    shift
    env "$@" ./efs-fuse "$IP:$P1" "$IP:$P2" "$IP:$P3" "$EXPORT" "$MNT" -f \
        >"$log" 2>&1 &
    CPID=$!
    for i in $(seq 1 40); do
        mountpoint -q "$MNT" && return 0
        sleep 0.25
    done
    echo "mount failed"; cat "$log"; return 1
}

unmount_fuse() {
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    if [ -n "${CPID:-}" ]; then
        wait "$CPID" 2>/dev/null || true
        CPID=
    fi
    sleep 0.3
}

verify_files() {
    local tag=$1
    local a b
    a=$(timeout 10 cat "$MNT/a.txt" 2>/dev/null || echo MISSING)
    b=$(timeout 10 cat "$MNT/b.txt" 2>/dev/null || echo MISSING)
    if [ "$a" = "alpha" ] && [ "$b" = "bravo" ]; then
        pass "$tag files readable"
    else
        fail "$tag files a='$a' b='$b'"
    fi
}

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$OUT"
: >"$REVIEW"
cd "$REPO"

echo "smoke-meta-edge on $(hostname) LOCAL=$LOCAL" | tee -a "$REVIEW"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

echo "=== build ==="
make -j"$(nproc)" efsd efs-fuse efs-mgmt efs-query

echo "=== start cluster ==="
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" --writers 4 \
    >"$OUT/s1.stdout" 2>&1 &
S1=$!
wait_port "$P1" || { cat "$OUT/s1.stdout"; exit 1; }

./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --writers 4 \
    --join "$IP:$P1" >"$OUT/s2.stdout" 2>&1 &
S2=$!
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --writers 4 \
    --join "$IP:$P1" >"$OUT/s3.stdout" 2>&1 &
S3=$!
wait_port "$P2" && wait_port "$P3" || { cat "$OUT"/s*.stdout; exit 1; }
sleep 0.5
./efs-mgmt mkfs "$IP:$P1" "$EXPORT"

EFS_META_BATCH_OPS=1 mount_fuse "$OUT/c0.stdout"
echo "alpha" >"$MNT/a.txt"
echo "bravo" >"$MNT/b.txt"
for i in $(seq 1 15); do echo "x$i" >"$MNT/n$i.txt"; done
sync
unmount_fuse

BEFORE_S2=$(count_meta_frags "$LOCAL/s2")
note "baseline s2 meta frags=$BEFORE_S2"
[ "${BEFORE_S2:-0}" -ge 1 ] && pass "A prep: s2 has meta frags" \
    || fail "A prep: s2 has no meta frags"

# ---------- A: wipe meta frags offline ----------
echo "=== A: wipe s2 meta frags offline ===" | tee -a "$REVIEW"
stop_s2
wipe_meta_frags "$LOCAL/s2"
AFTER_WIPE=$(count_meta_frags "$LOCAL/s2")
note "A after wipe s2 meta frags=$AFTER_WIPE"
[ "${AFTER_WIPE:-0}" -eq 0 ] && pass "A wiped s2 meta frags" || fail "A wipe incomplete ($AFTER_WIPE)"

start_s2
if wait_log "$OUT/s2.stdout" 'meta-heal:' 40; then
    pass "A meta-heal after wipe+restart"
    grep 'meta-heal:' "$OUT/s2.stdout" | tee -a "$REVIEW" | head -5
else
    fail "A no meta-heal after wipe+restart"
    tail -40 "$OUT/s2.stdout" | tee -a "$REVIEW" || true
fi
HEALED_A=$(count_meta_frags "$LOCAL/s2")
note "A after heal s2 meta frags=$HEALED_A"
[ "${HEALED_A:-0}" -ge 1 ] && pass "A local frags restored ($HEALED_A)" \
    || fail "A local frags still missing"

timeout 10 ./efs-query "$IP:$P2" >"$OUT/query-A.txt" 2>&1 \
    && pass "A efs-query via s2" || fail "A efs-query via s2"
EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cA.stdout"
verify_files "A"
unmount_fuse

# ---------- B: corrupt meta frags offline ----------
echo "=== B: corrupt s2 meta frags offline ===" | tee -a "$REVIEW"
stop_s2
corrupt_meta_frags "$LOCAL/s2"
# Truncate s2 log marker so we only match new heals
cp "$OUT/s2.stdout" "$OUT/s2.beforeB"
start_s2
: >"$OUT/s2.B.stdout"
# Append-only: wait for NEW heal lines vs beforeB
if wait_log "$OUT/s2.stdout" 'checksum mismatch|meta-heal:' 40; then
    NEW=$(comm -13 <(sort "$OUT/s2.beforeB") <(sort "$OUT/s2.stdout") | grep -E 'checksum mismatch|meta-heal:' || true)
    echo "$NEW" | tee -a "$REVIEW" | head -10
    if echo "$NEW" | grep -q 'meta-heal:'; then
        pass "B meta-heal after corruption"
    elif echo "$NEW" | grep -q 'checksum mismatch'; then
        # heal should follow; give catchup a moment
        if wait_log "$OUT/s2.stdout" 'meta-heal:' 20; then
            pass "B meta-heal after checksum mismatch"
        else
            fail "B saw checksum mismatch but no heal"
        fi
    else
        fail "B unexpected log"
    fi
else
    fail "B no mismatch/heal after corruption"
    tail -40 "$OUT/s2.stdout" | tee -a "$REVIEW" || true
fi
timeout 10 ./efs-query "$IP:$P2" >"$OUT/query-B.txt" 2>&1 \
    && pass "B efs-query via s2" || fail "B efs-query via s2"
EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cB.stdout"
verify_files "B"
unmount_fuse

# ---------- C: delete metadata.bin only ----------
echo "=== C: delete s2 metadata.bin (force peer root fetch) ===" | tee -a "$REVIEW"
stop_s2
META_BIN="$LOCAL/s2/meta/exports/$EXPORT/metadata.bin"
if [ -f "$META_BIN" ]; then
    cp -f "$META_BIN" "$OUT/s2.metadata.bin.bak"
    rm -f "$META_BIN"
    pass "C removed s2 metadata.bin"
else
    fail "C metadata.bin missing before delete"
fi
# Also wipe frags so join must fetch pages from peers
wipe_meta_frags "$LOCAL/s2"
start_s2
if wait_log "$OUT/s2.stdout" 'meta-heal:|Joined cluster|installed newer root' 40; then
    pass "C s2 rejoined / healed after metadata.bin loss"
    grep -E 'meta-heal:|meta-catchup:|Joined|Loaded' "$OUT/s2.stdout" \
        | tee -a "$REVIEW" | tail -15
else
    fail "C s2 did not recover after metadata.bin loss"
    tail -50 "$OUT/s2.stdout" | tee -a "$REVIEW" || true
fi
timeout 10 ./efs-query "$IP:$P2" >"$OUT/query-C.txt" 2>&1 \
    && pass "C efs-query via s2" || fail "C efs-query via s2"
EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cC.stdout"
verify_files "C"
# Advance gen while mounted for case D prep
echo "charlie" >"$MNT/c.txt"
for i in $(seq 16 25); do echo "y$i" >"$MNT/n$i.txt"; done
sync
sleep 0.5

# ---------- D: live wipe + PUT_META to trigger catchup ----------
echo "=== D: live wipe s2 meta frags then write (PUT_META) ===" | tee -a "$REVIEW"
BEFORE_D=$(count_meta_frags "$LOCAL/s2")
wipe_meta_frags "$LOCAL/s2"
note "D wiped live s2 meta frags (was $BEFORE_D, now $(count_meta_frags "$LOCAL/s2"))"
# Mark log position
MARK=$(wc -l <"$OUT/s2.stdout")
echo "delta" >"$MNT/d.txt"
for i in $(seq 26 35); do echo "z$i" >"$MNT/n$i.txt"; done
sync
unmount_fuse
# Wait for catchup heal after PUT_META from client flush
HEAL_D=0
for i in $(seq 1 40); do
    NEWLINES=$(tail -n +"$((MARK + 1))" "$OUT/s2.stdout" 2>/dev/null || true)
    if echo "$NEWLINES" | grep -q 'meta-heal:'; then
        HEAL_D=1
        echo "$NEWLINES" | grep -E 'meta-heal:|meta-catchup:' | tee -a "$REVIEW" | head -10
        break
    fi
    sleep 0.5
done
if [ "$HEAL_D" = "1" ]; then
    pass "D live heal after PUT_META"
else
    # Document gap if in-memory tables stayed valid without rebuild
    note "D no meta-heal within 20s; checking frag count / query"
    AFTER_D=$(count_meta_frags "$LOCAL/s2")
    note "D s2 meta frags after wait=$AFTER_D"
    if [ "${AFTER_D:-0}" -ge 1 ]; then
        pass "D frags present (heal may have raced without new log match)"
    else
        fail "D frags still missing after live wipe+writes"
        tail -n +"$((MARK + 1))" "$OUT/s2.stdout" | tee -a "$REVIEW" | tail -30 || true
    fi
fi
timeout 10 ./efs-query "$IP:$P2" >"$OUT/query-D.txt" 2>&1 \
    && pass "D efs-query via s2" || fail "D efs-query via s2"
EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cD.stdout"
verify_files "D"
[ "$(timeout 10 cat "$MNT/d.txt" 2>/dev/null || echo x)" = "delta" ] \
    && pass "D d.txt readable" || fail "D d.txt"
unmount_fuse

# ---------- E: dual-slot abort then break s2 ----------
echo "=== E: mid-flush abort then break/recover s2 ===" | tee -a "$REVIEW"
EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cE1.stdout"
echo "echo" >"$MNT/e.txt"
sync
unmount_fuse

# Abort mid next flush (inactive slot written, root not flipped)
EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cE2.stdout" EFS_META_FLUSH_ABORT_AFTER_PAGES=1 || true
# Client may die from abort; ensure unmounted
unmount_fuse
if grep -q "EFS_META_FLUSH_ABORT_AFTER_PAGES" "$OUT/cE2.stdout" 2>/dev/null; then
    pass "E abort hook fired"
else
    note "E abort hook not seen (may still exercise recover)"
fi

stop_s2
wipe_meta_frags "$LOCAL/s2"
rm -f "$LOCAL/s2/meta/exports/$EXPORT/metadata.bin"
start_s2
if wait_log "$OUT/s2.stdout" 'meta-heal:|meta-catchup:|Joined cluster' 40; then
    pass "E s2 recovered after abort+wipe"
    grep -E 'meta-heal:|meta-catchup:|Joined|legacy|decode' "$OUT/s2.stdout" \
        | tee -a "$REVIEW" | tail -20
else
    fail "E s2 did not recover"
    tail -50 "$OUT/s2.stdout" | tee -a "$REVIEW" || true
fi

EFS_META_BATCH_OPS=1 mount_fuse "$OUT/cE3.stdout"
verify_files "E"
[ "$(timeout 10 cat "$MNT/e.txt" 2>/dev/null || echo x)" = "echo" ] \
    && pass "E e.txt readable (prior gen)" || fail "E e.txt"
unmount_fuse

echo "=== summary ===" | tee -a "$REVIEW"
if [ "$FAIL" -ne 0 ]; then
    echo "SMOKE_FAIL" | tee -a "$REVIEW"
    exit 1
fi
echo "SMOKE_OK" | tee -a "$REVIEW"
exit 0
