#!/bin/bash
# FUSE integration test for efs.
# Starts three servers, forms a cluster, mounts the filesystem, writes a file,
# reads it back, kills one server, and verifies the file is still readable.

set -e

BASE="${EFS_TEST_BASE:-/tmp/efs_fuse_test}"
MNT="$BASE/mnt"

cleanup() {
    set +e
    fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null
    pkill -9 -x efs-fuse
    pkill -9 -x efsd
    sleep 0.5
    rm -rf "$BASE"
}

trap cleanup EXIT

cleanup
mkdir -p "$BASE/s1" "$BASE/s2" "$BASE/s3" "$MNT"

cd "$(dirname "$0")/.."

echo "Starting servers..."
./efsd --node-id 1 --addr 127.0.0.1 --port 17432 --storage "$BASE/s1" > "$BASE/s1.log" 2>&1 &
./efsd --node-id 2 --addr 127.0.0.1 --port 17433 --storage "$BASE/s2" > "$BASE/s2.log" 2>&1 &
./efsd --node-id 3 --addr 127.0.0.1 --port 17434 --storage "$BASE/s3" > "$BASE/s3.log" 2>&1 &

sleep 2

echo "Forming cluster..."
./efs-mgmt add-node 127.0.0.1:17433 127.0.0.1:17432
./efs-mgmt add-node 127.0.0.1:17434 127.0.0.1:17432
./efs-mgmt mkfs 127.0.0.1:17432 test

echo "Mounting FUSE client..."
./efs-fuse 127.0.0.1:17432 127.0.0.1:17433 127.0.0.1:17434 test "$MNT" -f > "$BASE/fuse.log" 2>&1 &
sleep 2

echo "Writing and reading..."
DATA="hello from efs fuse"
echo "$DATA" > "$MNT/file.txt"
READ_BACK=$(cat "$MNT/file.txt")
if [ "$READ_BACK" != "$DATA" ]; then
    echo "FAIL: read back mismatch: got '$READ_BACK', expected '$DATA'"
    exit 1
fi

echo "Changing attributes..."
chmod 700 "$MNT/file.txt"
PERM=$(stat -c %a "$MNT/file.txt")
if [ "$PERM" != "700" ]; then
    echo "FAIL: chmod mismatch: got $PERM, expected 700"
    exit 1
fi

# chown to the current user is a no-op but still exercises the FUSE chown path.
chown "$(id -u):$(id -g)" "$MNT/file.txt"

# Exercise utimens via the classic utime(2) syscall (kernel maps it).
UTIME_SRC="$BASE/utime_helper.c"
cat > "$UTIME_SRC" <<'EOF'
#include <utime.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    struct utimbuf u = { .actime = 1000000000, .modtime = 1000000000 };
    return utime(argv[1], &u);
}
EOF
gcc -O2 -o "$BASE/utime_helper" "$UTIME_SRC"
"$BASE/utime_helper" "$MNT/file.txt"
MTIME=$(stat -c %Y "$MNT/file.txt")
if [ "$MTIME" != "1000000000" ]; then
    echo "FAIL: utime mismatch: got $MTIME, expected 1000000000"
    exit 1
fi

echo "Truncating file..."
EXPECTED_AFTER_TRUNC="hello"
truncate -s 5 "$MNT/file.txt"
SIZE=$(stat -c %s "$MNT/file.txt")
if [ "$SIZE" != "5" ]; then
    echo "FAIL: truncate mismatch: got $SIZE, expected 5"
    exit 1
fi
READ_BACK=$(head -c 5 "$MNT/file.txt")
if [ "$READ_BACK" != "$EXPECTED_AFTER_TRUNC" ]; then
    echo "FAIL: read after truncate mismatch: got '$READ_BACK', expected '$EXPECTED_AFTER_TRUNC'"
    exit 1
fi

echo "Renaming file..."
mv "$MNT/file.txt" "$MNT/renamed.txt"
if [ -e "$MNT/file.txt" ]; then
    echo "FAIL: source file still exists after rename"
    exit 1
fi
READ_BACK=$(cat "$MNT/renamed.txt")
if [ "$READ_BACK" != "$EXPECTED_AFTER_TRUNC" ]; then
    echo "FAIL: read after rename mismatch: got '$READ_BACK', expected '$EXPECTED_AFTER_TRUNC'"
    exit 1
fi

echo "Virtual .stats (lookup-only)..."
# Must not appear in directory listings (including ls -a).
if ls -a "$MNT" | grep -qx '\.stats'; then
    echo "FAIL: .stats listed by readdir/ls -a"
    exit 1
fi
# Explicit path must work.
if [ ! -f "$MNT/.stats" ]; then
    echo "FAIL: .stats missing via direct getattr"
    exit 1
fi
STATS=$(cat "$MNT/.stats")
echo "$STATS" | grep -q 'imm_files=' || { echo "FAIL: .stats missing imm_files"; exit 1; }
echo "$STATS" | grep -q 'tree_files=' || { echo "FAIL: .stats missing tree_files"; exit 1; }
# After rename we have one regular file at root; .stats itself is not counted.
echo "$STATS" | grep -q 'imm_files=1' || {
    echo "FAIL: expected imm_files=1 in .stats (virtual file must not be counted):"
    echo "$STATS"
    exit 1
}

echo "Killing server 2..."
pkill -f 'efsd --node-id 2 --addr 127.0.0.1 --port 17433'
sleep 2

READ_BACK=$(cat "$MNT/renamed.txt")
if [ "$READ_BACK" != "$EXPECTED_AFTER_TRUNC" ]; then
    echo "FAIL: read after node failure mismatch: got '$READ_BACK', expected '$EXPECTED_AFTER_TRUNC'"
    exit 1
fi

echo "OK"
