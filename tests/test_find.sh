#!/bin/bash
# Functional test for the per-directory .find virtual directory and the
# efs-mgmt feature switches (.stats / .find). Starts a 3-node loopback cluster,
# mounts FUSE, builds a known tree, and exercises the single-command query
# interface:  cat "<dir>/.find/<term>"  -> newline-separated matching paths.
# Results are host-absolute (mountpoint + fs-root path) so they pipe/loop from
# any working directory. Covers the 4 glob forms, the min-term rule, recursion,
# piping, and the on/off toggles.

set -u
BASE="${EFS_TEST_BASE:-/tmp/efs_find_test}"
MNT="$BASE/mnt"
P1=19432; P2=19433; P3=19434
IP=127.0.0.1
FAIL=0

cleanup() {
    set +e
    fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
    pkill -9 -x efs-fuse 2>/dev/null
    pkill -9 -x efsd 2>/dev/null
    sleep 0.5
    rm -rf "$BASE"
}
trap cleanup EXIT

note() { echo "[find] $*"; }
check() { # check <desc> <expected> <actual>
    if [ "$2" = "$3" ]; then note "OK: $1"; else note "FAIL: $1"; echo "  want: $2"; echo "  got:  $3"; FAIL=1; fi
}

cleanup
mkdir -p "$BASE/s1" "$BASE/s2" "$BASE/s3" "$MNT"
cd "$(dirname "$0")/.."

note "Starting servers + cluster + mount..."
./efsd --node-id 1 --addr $IP --port $P1 --storage "$BASE/s1" >"$BASE/s1.log" 2>&1 &
./efsd --node-id 2 --addr $IP --port $P2 --storage "$BASE/s2" >"$BASE/s2.log" 2>&1 &
./efsd --node-id 3 --addr $IP --port $P3 --storage "$BASE/s3" >"$BASE/s3.log" 2>&1 &
sleep 2
./efs-mgmt add-node $IP:$P2 $IP:$P1 >/dev/null 2>&1
./efs-mgmt add-node $IP:$P3 $IP:$P1 >/dev/null 2>&1
./efs-mgmt mkfs $IP:$P1 test >/dev/null 2>&1
EFS_FEATURES_TTL_MS=300 ./efs-fuse $IP:$P1 $IP:$P2 $IP:$P3 test "$MNT" -f >"$BASE/fuse.log" 2>&1 &
sleep 2

# Build a known tree.
mkdir -p "$MNT/sub/deep"
echo x > "$MNT/report2024.txt"
echo x > "$MNT/report2025.txt"
echo x > "$MNT/sub/report_old.txt"
echo x > "$MNT/sub/deep/final_report.pdf"
echo x > "$MNT/other.log"
sync

# Results are host-absolute: canonical mountpoint + fs-root path.
MP=$(realpath "$MNT")

# --- 1. exact match ---
OUT=$(cat "$MNT/.find/report2024.txt")
check "exact match" "$MP/report2024.txt" "$OUT"

# --- 2. prefix match (recursive) ---
OUT=$(cat "$MNT/.find/report*" | sort)
want=$(printf '%s\n' "$MP/report2024.txt" "$MP/report2025.txt" "$MP/sub/report_old.txt" | sort)
check "prefix * recursive" "$want" "$OUT"

# --- 3. suffix match ---
OUT=$(cat "$MNT/.find/*2025.txt")
check "suffix match" "$MP/report2025.txt" "$OUT"

# --- 4. substring match (recursive, includes subdir file) ---
OUT=$(cat "$MNT/.find/*report*" | sort)
want=$(printf '%s\n' "$MP/report2024.txt" "$MP/report2025.txt" "$MP/sub/deep/final_report.pdf" "$MP/sub/report_old.txt" | sort)
check "substring recursive" "$want" "$OUT"

# --- 5. min term: 3 chars rejected, 4 accepted ---
if cat "$MNT/.find/abc" >/dev/null 2>&1; then
    note "FAIL: 3-char term accepted (want ENOENT)"; FAIL=1
else
    note "OK: 3-char term rejected"
fi
OUT=$(cat "$MNT/.find/repo*" | sort)   # 'repo' = 4 chars, prefix
want=$(printf '%s\n' "$MP/report2024.txt" "$MP/report2025.txt" "$MP/sub/report_old.txt" | sort)
check "4-char prefix ok" "$want" "$OUT"

# --- 6. subdirectory scoping: search under sub/ only (still host-absolute) ---
OUT=$(cat "$MNT/sub/.find/*report*" | sort)
want=$(printf '%s\n' "$MP/sub/deep/final_report.pdf" "$MP/sub/report_old.txt" | sort)
check "subdir scope (host-absolute)" "$want" "$OUT"

# --- 7. piping results to another command (works from any CWD: absolute) ---
N=$(cat "$MNT/.find/*2024.txt" | wc -l)
check "pipe to wc -l" "1" "$N"
# pipe absolute paths into ls -l from an unrelated directory
( cd / && cat "$MNT/.find/report*" | xargs ls -l >/dev/null 2>&1 ) \
    && note "OK: pipe to xargs ls (from /)" || { note "FAIL: pipe to xargs ls"; FAIL=1; }
# build a for loop over the result list
LOOPN=0
for f in $(cat "$MNT/.find/*report*"); do [ -f "$f" ] && LOOPN=$((LOOPN+1)); done
check "for-loop over results (absolute -f)" "4" "$LOOPN"

# --- 8. feature show / toggles ---
FEAT=$(./efs-mgmt feature $IP:$P1 test show 2>&1)
echo "$FEAT" | grep -q ".find=on" && note "OK: feature show find=on" || { note "FAIL: feature show: $FEAT"; FAIL=1; }

./efs-mgmt feature $IP:$P1 test find off >/dev/null 2>&1
sleep 1
if cat "$MNT/.find/report*" >/dev/null 2>&1; then
    note "FAIL: .find still readable after feature off"; FAIL=1
else
    note "OK: .find hidden when feature off"
fi
# .stats must still work while only find is off
cat "$MNT/.stats" >/dev/null 2>&1 && note "OK: .stats unaffected by find off" || { note "FAIL: .stats broke"; FAIL=1; }

./efs-mgmt feature $IP:$P1 test find on >/dev/null 2>&1
sleep 1
OUT=$(cat "$MNT/.find/report*" 2>/dev/null | sort | head -1)
check ".find back after feature on" "$MP/report2024.txt" "$OUT"

# stats off independently
./efs-mgmt feature $IP:$P1 test stats off >/dev/null 2>&1
sleep 1
if cat "$MNT/.stats" >/dev/null 2>&1; then
    note "FAIL: .stats still readable after stats off"; FAIL=1
else
    note "OK: .stats hidden when stats off"
fi
OUT=$(cat "$MNT/.find/report*" 2>/dev/null)
[ -n "$OUT" ] && note "OK: .find works while stats off" || { note "FAIL: .find broke while stats off"; FAIL=1; }
./efs-mgmt feature $IP:$P1 test stats on >/dev/null 2>&1

echo
if [ "$FAIL" = 0 ]; then note "ALL PASS"; else note "FAILURES"; fi
exit $FAIL
