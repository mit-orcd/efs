#!/bin/bash
# k05_session_fence_gate.sh REL DIR [PORT]
#
# K5 (W134 with W72) cluster gate: a private 3-daemon TCP cluster, two FUSE
# mounts A and B, the core POSIX suite on A, then `efs-mgmt raft-session
# fence` of A's mount session. Every later mutation on A must fail EIO
# (FENCED, never resent under the old epoch) while reads on A still work and
# B keeps mutating normally. Ends by stopping everything and deleting DIR.
#
# REL is a built tree (efsd, efs-fuse, efs-mgmt, scripts/, tests/). DIR must
# not exist or be empty and be writable by this user (on efs2:
# `sudo install -d -o $USER /data1/k05`); it is deleted at the end. Ports PORT+1..PORT+3
# (default 18600 -> 18601-18603). Buffered I/O (--no-direct-io), TCP only.
# Never point this at a shared cluster: it formats its own stores.
#
# Output: one "CHECK <name> PASS|FAIL <detail>" line per check and a final
# "GATE PASS|FAIL". Raw daemon/client logs stay in DIR/logs until cleanup;
# set K05_KEEP=1 to keep DIR (e.g. to inspect a failure).
set -u
REL=$(cd "$1" && pwd); D=$2; PORT=${3:-18600}
ADDR=127.0.0.1
export EFS_TRANSPORT=tcp
POSIX_JOBS=${K05_POSIX_JOBS:-2}
fails=0
check() { # name rc detail
    if [ "$2" = 0 ]; then echo "CHECK $1 PASS ${3:-}"; else echo "CHECK $1 FAIL ${3:-}"; fails=$((fails+1)); fi
}
stop_all() {
    # A fenced mount can never publish its unwritten data (that is the point
    # of the fence), so A is detached with an explicit discard.
    (cd "$REL" && ./scripts/client.sh stop --force-discard "$D/mntA" >/dev/null 2>&1)
    (cd "$REL" && ./scripts/client.sh stop "$D/mntB" >/dev/null 2>&1)
    for m in "$D/mntA" "$D/mntB"; do mountpoint -q "$m" && fusermount3 -uz "$m" 2>/dev/null; done
    for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
    sleep 3
    for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
}
cleanup() {
    stop_all
    if [ "${K05_KEEP:-0}" = 1 ]; then echo "kept $D"; return; fi
    if command -v edelete >/dev/null 2>&1; then
        [ ! -e "$D" ] || edelete --force --delete "$D" >/dev/null
    else
        rm -rf -- "$D"
    fi
    rm -f "$D"/.efs-mnt?.lock 2>/dev/null
    # The parent (e.g. /data1) may only be writable by root; DIR was then
    # created by root and handed to this user, so remove the empty top too.
    [ ! -e "$D" ] || rmdir "$D" 2>/dev/null || sudo -n rmdir "$D" 2>/dev/null
    [ ! -e "$D" ] && echo "deleted $D"
}
trap cleanup EXIT

for i in 1 2 3; do
    ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; trap - EXIT; exit 1; }
done
rm -f "$D"/.efs-mnt?.lock 2>/dev/null # client.sh lock files of an earlier run
[ ! -e "$D" ] || [ -z "$(ls -A "$D")" ] || { echo "$D is not empty"; trap - EXIT; exit 1; }
mkdir -p "$D"/{n1,n2,n3,logs,mntA,mntB}
(cd "$REL" && sha256sum efsd efs-fuse efs-mgmt | sed 's/^/binary /')
for i in 1 2 3; do
    join=""; [ $i -gt 1 ] && join="--join $ADDR:$((PORT+1))"
    nohup "$REL/efsd" --node-id $i --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" \
        --quota 2G --no-direct-io $join >> "$D/logs/efsd-n$i.log" 2>&1 &
    echo $! > "$D/logs/n$i.pid"
    for t in $(seq 60); do grep -q listening "$D/logs/efsd-n$i.log" && break; sleep 0.5; done
done
ok=1
for t in $(seq 60); do "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && { ok=0; break; }; sleep 1; done
check mkfs $ok
for m in A B; do
    (cd "$REL" && EFS_FUSE_LOG="$D/logs/fuse-$m.log" ./scripts/client.sh $ADDR:$((PORT+1)) "$D/mnt$m" > "$D/logs/client-$m.out" 2>&1)
    findmnt -rn "$D/mnt$m" > /dev/null; check "mount-$m" $? "$D/mnt$m"
done
UA=$(grep -o 'session uuid=[0-9a-f]* epoch=[0-9]*' "$D/logs/fuse-A.log" | tail -1)
UB=$(grep -o 'session uuid=[0-9a-f]* epoch=[0-9]*' "$D/logs/fuse-B.log" | tail -1)
[ -n "$UA" ] && [ -n "$UB" ] && [ "$UA" != "$UB" ]; check mount-sessions $? "A: $UA; B: $UB"
uuidA=$(echo "$UA" | sed 's/.*uuid=\([0-9a-f]*\).*/\1/')
out=$("$REL/efs-mgmt" raft-session $ADDR:$((PORT+1)) get "$uuidA" 2>&1)
echo "$out" | grep -q 'rc=0 .*epoch=1 state=1'; check session-active-at-mount $? "$out"

# Core POSIX suite on A while its session is live.
t0=$(date +%s)
python3 "$REL/tests/posix/posix_suite.py" "$D/mntA" --suite core --jobs "$POSIX_JOBS" \
    --results "$D/logs/posix-A.txt" > "$D/logs/posix-A.out" 2>&1
rc=$?
summary=$(tail -2 "$D/logs/posix-A.out" | tr '\n' ' ')
check posix-core-A $rc "$((($(date +%s)-t0)))s $summary"
[ $rc = 0 ] || grep -E 'FAIL' "$D/logs/posix-A.out" | head -20

# Fixtures on A before the fence: a file to write, rename, unlink and lock,
# and a reader that opens g now and reads it through that descriptor after
# the fence (an open lease taken before the fence; a new open() after it is
# itself a session mutation and fails).
mkdir -p "$D/mntA/k05" && echo before > "$D/mntA/k05/f" && echo keep > "$D/mntA/k05/g" &&
    echo idle > "$D/mntA/k05/h"
check fixtures-A $?
sync
python3 - "$D/mntA/k05/g" "$D/logs/reader.go" "$D/logs/reader.out" <<'EOP' &
import os, sys, time
fd = os.open(sys.argv[1], os.O_RDONLY)
open(sys.argv[3] + ".ready", "w").close()
while not os.path.exists(sys.argv[2]): time.sleep(0.1)
try: data = os.pread(fd, 64, 0); open(sys.argv[3], "w").write(repr(data))
except OSError as e: open(sys.argv[3], "w").write("ERR %s" % e)
EOP
READER=$!
for t in $(seq 100); do [ -e "$D/logs/reader.out.ready" ] && break; sleep 0.1; done

# Fence A's mount session (global barrier: BEGIN, FENCE_LOC+ACK every touched
# shard, FINISH). The command is what an operator runs for a dead client. It
# is idempotent and refuses (BUSY) a participant whose session row still has
# an unresolved transaction intent from recent work; repeat it, bounded.
fence_ok=1
for attempt in $(seq 12); do
    out=$(G0_TO=60 "$REL/efs-mgmt" raft-session $ADDR:$((PORT+1)) fence "$uuidA" 2>&1)
    if echo "$out" | grep -q 'status=0' && echo "$out" | grep -q 'epoch=2' && echo "$out" | grep -q 'state=1'; then
        fence_ok=0; break
    fi
    echo "fence attempt $attempt: $out"
    sleep 10
done
check fence-A $fence_ok "attempts=$attempt $out"

# Every mutation on A now fails EIO; reads still work.
python3 - "$D/mntA/k05" <<'EOP'
import errno, fcntl, os, sys
d = sys.argv[1]
def expect_eio(name, fn):
    try:
        fn()
    except OSError as e:
        ok = e.errno == errno.EIO
        print("CHECK fenced-%s %s errno=%s" % (name, "PASS" if ok else "FAIL", errno.errorcode.get(e.errno, e.errno)))
        return ok
    print("CHECK fenced-%s FAIL succeeded" % name)
    return False
def write_sync():
    fd = os.open(os.path.join(d, "f"), os.O_WRONLY)
    try:
        os.write(fd, b"after-fence")
        os.fsync(fd)
    finally:
        try: os.close(fd)
        except OSError: pass
def flock():
    fd = os.open(os.path.join(d, "g"), os.O_RDONLY)
    try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    finally: os.close(fd)
results = [
    expect_eio("create", lambda: os.close(os.open(os.path.join(d, "new"), os.O_CREAT | os.O_WRONLY, 0o644))),
    expect_eio("mkdir", lambda: os.mkdir(os.path.join(d, "dir"))),
    expect_eio("rename", lambda: os.rename(os.path.join(d, "g"), os.path.join(d, "g2"))),
    expect_eio("unlink", lambda: os.unlink(os.path.join(d, "g"))),
    expect_eio("link", lambda: os.link(os.path.join(d, "g"), os.path.join(d, "g3"))),
    expect_eio("chmod", lambda: os.chmod(os.path.join(d, "g"), 0o600)),
    expect_eio("utime", lambda: os.utime(os.path.join(d, "g"), (1, 1))),
    expect_eio("truncate", lambda: os.truncate(os.path.join(d, "g"), 1)),
    expect_eio("setxattr", lambda: os.setxattr(os.path.join(d, "g"), "user.k05", b"x")),
    expect_eio("flock", flock),
    expect_eio("write-fsync", write_sync),
]
# h is held open by nobody: its first open needs a new open lease (I19).
results.append(expect_eio("open-new-lease", lambda: os.close(os.open(os.path.join(d, "h"), os.O_RDONLY))))
try:
    names = sorted(os.listdir(d))
    st = os.stat(os.path.join(d, "g"))
    ok = names == ["f", "g", "h"] and st.st_size == 5
    print("CHECK fenced-metadata-reads %s names=%s size(g)=%d" % ("PASS" if ok else "FAIL", names, st.st_size))
    results.append(ok)
except OSError as e:
    print("CHECK fenced-metadata-reads FAIL %s" % e)
    results.append(False)
sys.exit(0 if all(results) else 1)
EOP
[ $? = 0 ] || fails=$((fails+1))
touch "$D/logs/reader.go"; wait $READER
grep -q "b'keep" "$D/logs/reader.out"; check fenced-read-open-fd $? "$(cat "$D/logs/reader.out" 2>/dev/null)"
grep -q "session $uuidA epoch 1 fenced" "$D/logs/fuse-A.log"; check fence-latched-in-client-log $? "$(grep -m1 'fenced' "$D/logs/fuse-A.log")"

# B keeps working, and sees A's pre-fence state.
python3 - "$D/mntB/k05" <<'EOP'
import os, sys
d = sys.argv[1]
try:
    p = os.path.join(d, "fromB")
    with open(p, "w") as f:
        f.write("b-works\n"); f.flush(); os.fsync(f.fileno())
    os.mkdir(os.path.join(d, "dirB"))
    os.rename(p, os.path.join(d, "fromB2"))
    os.chmod(os.path.join(d, "fromB2"), 0o600)
    os.setxattr(os.path.join(d, "fromB2"), "user.k05", b"b")
    assert open(os.path.join(d, "fromB2")).read() == "b-works\n"
    assert open(os.path.join(d, "f")).read().startswith("before")
    os.unlink(os.path.join(d, "fromB2")); os.rmdir(os.path.join(d, "dirB"))
    print("CHECK peer-B-mutates PASS create/fsync/mkdir/rename/chmod/setxattr/unlink/rmdir")
except Exception as e:
    print("CHECK peer-B-mutates FAIL %r" % (e,)); sys.exit(1)
EOP
[ $? = 0 ] || fails=$((fails+1))
out=$("$REL/efs-mgmt" raft-session $ADDR:$((PORT+1)) get "$uuidA" 2>&1)
echo "$out" | grep -q 'rc=0 .*epoch=2 state=1'; check session-A-epoch-2 $? "$out"

if [ $fails = 0 ]; then echo "GATE PASS"; else echo "GATE FAIL ($fails)"; fi
exit $((fails != 0))
