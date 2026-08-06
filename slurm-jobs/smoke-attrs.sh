#!/bin/bash
#SBATCH --job-name=efs-smoke-attrs
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-attrs-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-attrs-%j.err
#
# Smoke: chmod, chown, chgrp, ln (hard), ln -s (symlink).

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-attrs-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1971
P2=1972
P3=1973
MNT="$LOCAL/mnt"
FAIL=0

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

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

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$OUT"

echo "smoke-attrs on $(hostname) LOCAL=$LOCAL"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
    > "$OUT/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    if grep -q "listening on" "$OUT/s1.stdout" 2>/dev/null; then break; fi
    if ! kill -0 "$S1" 2>/dev/null; then
        echo "server 1 died:"; cat "$OUT/s1.stdout"; exit 1
    fi
    sleep 0.25
done
grep -q "listening on" "$OUT/s1.stdout" || { echo "s1 not ready"; cat "$OUT/s1.stdout"; exit 1; }

"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    > "$OUT/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    > "$OUT/s3.stdout" 2>&1 &
S3=$!

for port in "$P2" "$P3"; do
    READY=0
    for i in $(seq 1 40); do
        if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then READY=1; break; fi
        sleep 0.25
    done
    [ "$READY" = "1" ] || { echo "server :$port failed"; cat "$OUT"/s*.stdout; exit 1; }
done
sleep 0.5

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" attrsmoke 2>&1)"

# Flush meta every op so attr changes are durable during the smoke.
EFS_META_BATCH_OPS=1 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" attrsmoke "$MNT" -f \
    > "$OUT/client.stdout" 2>&1 &
CPID=$!

MOUNTED=0
for i in $(seq 1 40); do
    if mountpoint -q "$MNT" 2>/dev/null; then MOUNTED=1; break; fi
    if ! kill -0 "$CPID" 2>/dev/null; then
        echo "client died:"; cat "$OUT/client.stdout"; exit 1
    fi
    sleep 0.25
done
[ "$MOUNTED" = "1" ] || { echo "mount failed"; cat "$OUT/client.stdout"; exit 1; }
echo "mounted"

ME=$(id -u)
MG=$(id -g)
GN=$(id -gn)

echo "=== chmod ==="
timeout 15 bash -c "echo hello > '$MNT/a.txt'" || fail "create a.txt"
timeout 15 chmod 644 "$MNT/a.txt" || fail "chmod 644"
MODE=$(timeout 15 stat -c '%a' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$MODE" = "644" ] && pass "chmod 644 -> $MODE" || fail "chmod 644 got $MODE"
timeout 15 chmod 600 "$MNT/a.txt" || fail "chmod 600"
MODE=$(timeout 15 stat -c '%a' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$MODE" = "600" ] && pass "chmod 600 -> $MODE" || fail "chmod 600 got $MODE"
timeout 15 chmod 755 "$MNT/a.txt" || fail "chmod 755"
MODE=$(timeout 15 stat -c '%a' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$MODE" = "755" ] && pass "chmod 755 -> $MODE" || fail "chmod 755 got $MODE"

echo "=== chown / chgrp ==="
timeout 15 chown "$ME" "$MNT/a.txt" || fail "chown uid"
UIDG=$(timeout 15 stat -c '%u:%g' "$MNT/a.txt" 2>/dev/null || echo missing)
echo "stat after chown: $UIDG"
[[ "$UIDG" == "$ME:"* ]] && pass "chown uid" || fail "chown uid got $UIDG"

timeout 15 chgrp "$GN" "$MNT/a.txt" || fail "chgrp"
GID=$(timeout 15 stat -c '%g' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$GID" = "$MG" ] && pass "chgrp -> $GID" || fail "chgrp got $GID want $MG"

timeout 15 chown "$ME:$MG" "$MNT/a.txt" || fail "chown uid:gid"
UIDG=$(timeout 15 stat -c '%u:%g' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$UIDG" = "$ME:$MG" ] && pass "chown uid:gid" || fail "chown uid:gid got $UIDG"

echo "=== hard link (ln) ==="
timeout 15 ln "$MNT/a.txt" "$MNT/a-hard.txt" || fail "ln hard"
N1=$(timeout 15 stat -c '%i %h' "$MNT/a.txt" 2>/dev/null || echo missing)
N2=$(timeout 15 stat -c '%i %h' "$MNT/a-hard.txt" 2>/dev/null || echo missing)
echo "a.txt=$N1 a-hard=$N2"
INO1=${N1%% *}; NL1=${N1##* }
INO2=${N2%% *}; NL2=${N2##* }
[ "$INO1" = "$INO2" ] && [ "$NL1" = "2" ] && [ "$NL2" = "2" ] \
    && pass "hardlink same ino nlink=2" \
    || fail "hardlink ino/nlink ($N1 vs $N2)"
CONTENT=$(timeout 15 cat "$MNT/a-hard.txt" 2>/dev/null || echo missing)
[ "$CONTENT" = "hello" ] && pass "hardlink content" || fail "hardlink content=$CONTENT"
timeout 15 bash -c "echo world > '$MNT/a-hard.txt'" || fail "write via hardlink"
CONTENT=$(timeout 15 cat "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$CONTENT" = "world" ] && pass "hardlink shared data" || fail "shared data=$CONTENT"
timeout 15 rm -f "$MNT/a-hard.txt" || fail "unlink hardlink name"
N1=$(timeout 15 stat -c '%h' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$N1" = "1" ] && pass "nlink after unlink hard" || fail "nlink after unlink=$N1"
CONTENT=$(timeout 15 cat "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$CONTENT" = "world" ] && pass "data survives hard unlink" || fail "data after unlink=$CONTENT"

echo "=== symlink (ln -s) ==="
timeout 15 ln -s a.txt "$MNT/a-soft.txt" || fail "ln -s"
TARGET=$(timeout 15 readlink "$MNT/a-soft.txt" 2>/dev/null || echo missing)
[ "$TARGET" = "a.txt" ] && pass "readlink -> $TARGET" || fail "readlink=$TARGET"
CONTENT=$(timeout 15 cat "$MNT/a-soft.txt" 2>/dev/null || echo missing)
[ "$CONTENT" = "world" ] && pass "symlink follow read" || fail "symlink read=$CONTENT"
TYPE=$(timeout 15 stat -c '%F' "$MNT/a-soft.txt" 2>/dev/null || echo missing)
echo "symlink stat type: $TYPE"
LLTYPE=$(timeout 15 ls -l "$MNT/a-soft.txt" 2>/dev/null | awk '{print $1}' || echo missing)
echo "ls -l mode field: $LLTYPE"
[[ "$LLTYPE" == l* ]] && pass "symlink shows as link" || fail "not a symlink in ls ($LLTYPE)"

echo "=== hardlink in subdir + chmod via link ==="
timeout 15 mkdir -p "$MNT/sub" || fail "mkdir sub"
timeout 15 ln "$MNT/a.txt" "$MNT/sub/b.txt" || fail "ln into sub"
timeout 15 chmod 640 "$MNT/sub/b.txt" || fail "chmod via hardlink"
MODE=$(timeout 15 stat -c '%a' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$MODE" = "640" ] && pass "chmod via hardlink propagates" || fail "mode via hl=$MODE"
INOA=$(timeout 15 stat -c '%i' "$MNT/a.txt" 2>/dev/null || echo x)
INOB=$(timeout 15 stat -c '%i' "$MNT/sub/b.txt" 2>/dev/null || echo y)
[ "$INOA" = "$INOB" ] && pass "subdir hardlink same ino" || fail "subdir ino $INOA vs $INOB"

echo "=== absolute symlink + nested ==="
timeout 15 ln -s /a.txt "$MNT/abs-soft" || fail "ln -s absolute"
# absolute within this mount may not resolve; readlink must still return target
ATARGET=$(timeout 15 readlink "$MNT/abs-soft" 2>/dev/null || echo missing)
[ "$ATARGET" = "/a.txt" ] && pass "abs readlink" || fail "abs readlink=$ATARGET"
timeout 15 ln -s sub/b.txt "$MNT/rel-soft" || fail "ln -s relative"
RCONTENT=$(timeout 15 cat "$MNT/rel-soft" 2>/dev/null || echo missing)
[ "$RCONTENT" = "world" ] && pass "relative symlink follow" || fail "rel follow=$RCONTENT"

echo "=== chgrp via hardlink ==="
timeout 15 chgrp "$GN" "$MNT/sub/b.txt" || fail "chgrp via hardlink"
GID=$(timeout 15 stat -c '%g' "$MNT/a.txt" 2>/dev/null || echo missing)
[ "$GID" = "$MG" ] && pass "chgrp via hardlink" || fail "chgrp via hl gid=$GID"

echo "=== unlink last name removes file ==="
timeout 15 rm -f "$MNT/a.txt" "$MNT/sub/b.txt" || fail "rm both hard names"
if timeout 15 cat "$MNT/a.txt" >/dev/null 2>&1; then
    fail "a.txt still readable after last unlink"
else
    pass "gone after last hardlink unlink"
fi

echo "=== summary ==="
if [ "$FAIL" -eq 0 ]; then
    echo "SMOKE_ATTRS_OK"
    exit 0
fi
echo "SMOKE_ATTRS_FAIL"
exit 1
