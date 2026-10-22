#!/bin/bash
# d25_step4_gates.sh RELEASE_DIR LABEL
#
# D25 step 4 gates g5-g8 (sweep, retirement, clipping, packing) for the
# logical truncate, on a private
# 3-daemon cluster on one host with two FUSE mounts A and B (ports
# PORT+1..PORT+3, default 17671-17673; stores under ${D25_ROOT:-/tmp/d25-gates}
# /<label>, --direct-io, 64 GiB quota). Refuses to start when a port is in use;
# never touches orcd-mini (17432-17434).
#
# Needs an EFS_FAULTS=1 build (the sweep hold for g6). The packer runs with
# low thresholds for g8.
#
#   g5 a warm descriptor on B across a shrink, the sweep and retirement on A
#      reads new bytes and zeros, never discarded data; reopen matches
#   g6 space: truncate an allocated ALLOC_GIB file to 0, the sweep drains
#      (trunc_pending 0 on every node) and data usage falls; new writes made
#      during the sweep survive it; a restart of all daemons mid-sweep and the
#      sweep still completes; with the sweep held, 32 non-dominated shrinks
#      fill the history, the 33rd is refused, and after the hold lifts the
#      retried shrink succeeds
#   g7 spans and clipped boundaries across shrink/extend/rewrite, verified
#      before the sweep, after retirement, and cold
#   g8 PACK_FILES small files packed, some truncated (incl. to 0) and some
#      unlinked while the rest are read; verified after sweep and cold
#   then remount, restart all daemons, remount and compare every file.
set -u
REL=$1; L=$2
PORT=${PORT:-17850}; R=${D25_ROOT:-/tmp/d25-gates}/$L; A=${D25_ADDR:-127.0.0.1}
ALLOC_GIB=${ALLOC_GIB:-2}; PACK_FILES=${PACK_FILES:-400}
export EFS_TRANSPORT=${EFS_TRANSPORT:-tcp} EFS_MD_RAFT_N=3 ASAN_OPTIONS=detect_leaks=0
export EFS_FAULT_TRUNCATE_BEFORE_FILE=$R/fault-before EFS_FAULT_TRUNCATE_APPLY_FILE=$R/fault-apply
export EFS_FAULT_TRUNCATE_DROP_REPLY_FILE=$R/fault-drop EFS_FAULT_TRUNC_SWEEP_HOLD_FILE=$R/sweep-hold
export EFS_PACK_MIN_AGE_MS=500 EFS_PACK_SPACE_START_PCT=0 EFS_PACK_SPACE_FULL_PCT=1 EFS_PACK_COMPACT_IDLE_S=2 EFS_PACK_DBG=1
for i in 1 2 3; do
  ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "$L: port $((PORT+i)) in use"; exit 1; }
done
findmnt -rn -o TARGET | grep -q "^$R/" && { echo "$L: a mount under $R is still live"; exit 1; }
rm -rf "$R"; mkdir -p "$R"/{n1,n2,n3,logs,mA,mB}
start_daemons() {
  for i in 1 2 3; do
    join=""; [ $i -gt 1 ] && join="--join $A:$((PORT+1))"
    nohup "$REL/efsd" --node-id $i --addr "$A" --port $((PORT+i)) --storage "$R/n$i" \
      --quota 64G --direct-io $join >> "$R/logs/efsd-n$i.log" 2>&1 &
    echo $! > "$R/logs/n$i.pid"
    for t in $(seq 120); do grep -q listening "$R/logs/efsd-n$i.log" && break; sleep 0.5; done
    kill -0 "$(cat "$R/logs/n$i.pid")" 2>/dev/null || { echo "$L: efsd n$i exited"; tail -3 "$R/logs/efsd-n$i.log"; exit 1; }
  done
  for t in $(seq 60); do
    up=$("$REL/efs-mgmt" status $A:$((PORT+1)) 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
    [ "$up" -eq 3 ] && break; sleep 1
  done
}
stop_daemons() {
  for i in 1 2 3; do kill "$(cat "$R/logs/n$i.pid")" 2>/dev/null; done
  for t in $(seq 30); do
    alive=0; for i in 1 2 3; do kill -0 "$(cat "$R/logs/n$i.pid")" 2>/dev/null && alive=1; done
    [ $alive = 0 ] && return; sleep 1
  done
  for i in 1 2 3; do kill -9 "$(cat "$R/logs/n$i.pid")" 2>/dev/null; done
}
# A refused clean stop means unresolved writes: record it as a failure,
# then discard explicitly so the run can continue and clean up.
UNRESOLVED=0
umount_all() {
  for m in mA mB; do
    findmnt -rn "$R/$m" > /dev/null || continue
    if ! "$REL/scripts/client.sh" stop "$R/$m" >> "$R/logs/client-$m.out" 2>&1; then
      UNRESOLVED=$((UNRESOLVED+1)); echo "  FAIL  clean unmount of $m refused (unresolved writes); force-discarding"
      "$REL/scripts/client.sh" stop --force-discard "$R/$m" >> "$R/logs/client-$m.out" 2>&1
    fi
  done
}
mount_all() {
  for m in mA mB; do
    for t in $(seq 5); do
      EFS_FUSE_LOG="$R/logs/fuse-$m.log" "$REL/scripts/client.sh" $A:$((PORT+1)) "$R/$m" >> "$R/logs/client-$m.out" 2>&1
      findmnt -rn "$R/$m" > /dev/null && break; sleep 2
    done
    findmnt -rn "$R/$m" > /dev/null || { echo "$L: mount $m failed"; tail -5 "$R/logs/client-$m.out"; exit 1; }
  done
}
cleanup() {
  umount_all; stop_daemons
  [ "${D25_KEEP:-0}" = 1 ] || rm -rf "$R"/n1 "$R"/n2 "$R"/n3
}
trap cleanup EXIT
start_daemons
for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs $A:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
mount_all
echo "== $L $("$REL/efsd" --version | head -1) transport=$EFS_TRANSPORT"
export D25_REL=$REL D25_PORT=$PORT D25_ADDR=$A

cat > "$R/gates.py" <<'EOF'
import errno, hashlib, os, pickle, random, re, subprocess, sys, threading, time
mA, mB, R, phase = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
REL, PORT = os.environ["D25_REL"], int(os.environ["D25_PORT"])
MiB = 1 << 20; CS = 128 * 1024; rng = random.Random(4242)
fails = []
state = os.path.join(R, "models.pkl")
def check(name, ok, detail=""):
    print("  %-5s %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True); ok or fails.append(name)
def h(b): return hashlib.sha256(b).hexdigest()[:16]
def readall(p):
    with open(p, "rb") as f: return f.read()
def write_at(p, off, data, create=False):
    fd = os.open(p, os.O_RDWR | (os.O_CREAT if create else 0), 0o644)
    try:
        os.pwrite(fd, data, off); os.fsync(fd)
    finally: os.close(fd)
def same(name, path, model):
    got = readall(path)
    bad = next((i for i in range(min(len(got), len(model))) if got[i] != model[i]), -1) if got != model else -1
    check(name, got == model, "size=%d/%d sha=%s/%s first_diff=%d" % (len(got), len(model), h(got), h(model), bad))
def tr(model, n): return model[:n] + b"\0" * max(0, n - len(model))
def put(model, off, data):
    if off > len(model): model += b"\0" * (off - len(model))
    return model[:off] + data + model[off + len(data):]
def gc_status():
    # Every node must answer with both fields; a silent miss once made the
    # sweep waits and the space check vacuous.
    tot = {"pending": 0, "usage": 0}
    for i in (1, 2, 3):
        r = subprocess.run([REL + "/efs-mgmt", "gc-status", "%s:%d" % (os.environ["D25_ADDR"], PORT + i)],
                           capture_output=True, text=True)
        mm = re.search(r"trunc_pending group0=(\d+) group2=(\d+)", r.stdout)
        u = re.search(r"data_usage_bytes=(\d+)", r.stdout)
        if r.returncode or not mm or not u:
            raise SystemExit("gc-status unreadable on node %d: rc=%d %s" % (i, r.returncode, r.stderr.strip()[:200]))
        tot["pending"] = max(tot["pending"], int(mm.group(1)) + int(mm.group(2)))
        tot["usage"] += int(u.group(1))
    return tot
def wait_swept(label, timeout=300):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if gc_status()["pending"] == 0:
            time.sleep(2)  # one more pass: retirement follows the last SWEEP_DONE
            if gc_status()["pending"] == 0:
                return time.time() - t0
        time.sleep(1)
    check(label + ": sweep drained", False, "timeout %d s" % timeout)
    return -1
def hold(on):
    p = os.path.join(R, "sweep-hold")
    if on: open(p, "w").close()
    elif os.path.exists(p): os.unlink(p)
def m(n): return os.path.join(mA, n)
def b(n): return os.path.join(mB, n)
models = {}
if phase == "cold":
    models = pickle.load(open(state, "rb"))
    for name, model in sorted(models.items()):
        same("cold B " + name, b(name), model)
        same("cold A " + name, m(name), model)
    sys.exit(1 if fails else 0)

# g5: a warm descriptor across shrink, sweep and retirement.
f = os.urandom(32 * MiB); write_at(m("g5"), 0, f, create=True)
fd = os.open(b("g5"), os.O_RDONLY); warm = os.pread(fd, len(f), 0)
check("g5 warm read on B", warm == f)
os.truncate(m("g5"), 3 * MiB + 777); f = tr(f, 3 * MiB + 777)
os.truncate(m("g5"), 20 * MiB); f = tr(f, 20 * MiB)
dt = wait_swept("g5")
held = os.pread(fd, 32 * MiB, 0); os.close(fd)
check("g5 held fd after retirement: new bytes, zeros, never discarded",
      held[:len(f)] == f and held[len(f):] == b"\0" * (len(held) - len(f)),
      "swept in %.1f s, len=%d" % (dt, len(held)))
same("g5 reopen on B after retirement", b("g5"), f)
models["g5"] = f

# g6a: space returns after truncate to 0.
u0 = gc_status()["usage"]
fd = os.open(m("big"), os.O_RDWR | os.O_CREAT, 0o644); blk = os.urandom(4 * MiB)
gib = int(os.environ.get("ALLOC_GIB", "2"))
for k in range(gib * 256): os.write(fd, blk)
os.fsync(fd); os.close(fd)
time.sleep(3); u1 = gc_status()["usage"]
t = time.time(); fd = os.open(m("big"), os.O_WRONLY | os.O_TRUNC); dt = time.time() - t; os.close(fd)
# g6c: new-epoch writes while the sweep runs, across many lanes.
nb = b""
for k in range(0, 256, 7):
    d = os.urandom(5000); write_at(m("big"), k * CS + 3, d); nb = put(nb, k * CS + 3, d)
ds = wait_swept("g6 space", 600)
t0 = time.time(); u2 = u1
while time.time() - t0 < 300:
    u2 = gc_status()["usage"]
    if u1 - u2 >= 0.8 * (u1 - u0): break
    time.sleep(2)
check("g6 O_TRUNC of %d GiB, sweep drained, usage returned" % gib, u1 - u2 >= 0.8 * (u1 - u0),
      "otrunc=%.3f s sweep=%.1f s usage %d -> %d -> %d (%.0f%% reclaimed in %.0f s)" % (
      dt, ds, u0, u1, u2, 100.0 * (u1 - u2) / max(1, u1 - u0), time.time() - t0))
same("g6 new-epoch writes made during the sweep survive it", b("big"), nb)
models["big"] = nb

# g6d: the history bound while the sweep is held.
hold(True)
x = os.urandom(8 * MiB); write_at(m("hist"), 0, x, create=True)
refused = None
for k in range(1, 34):
    try:
        os.truncate(m("hist"), 8 * MiB); x = tr(x, 8 * MiB)   # extend (no entry)
        os.truncate(m("hist"), k * 1000)                       # non-dominated shrink
        x = tr(x, k * 1000)
    except OSError as e:
        refused = (k, e.errno); break
check("g6 33rd non-dominated shrink refused while the sweep is held",
      refused is not None and refused[0] == 33 and refused[1] in (errno.EBUSY, errno.EAGAIN), str(refused))
same("g6 refused shrink changed nothing", b("hist"), x)
hold(False)
t0 = time.time(); ok = False
while time.time() - t0 < 300:
    try:
        os.truncate(m("hist"), 33000); x = tr(x, 33000); ok = True; break
    except OSError:
        time.sleep(2)
check("g6 shrink succeeds once the sweep resumes", ok, "after %.1f s" % (time.time() - t0))
same("g6 history-bound file bytes", b("hist"), x)
models["hist"] = x

# g7: spans and clipped boundaries.
y = os.urandom(MiB); write_at(m("g7"), 0, y, create=True)
for off in (3 * CS + 100, 3 * CS + 9000, 3 * CS + 70000):
    d = os.urandom(3000); write_at(b("g7"), off, d); y = put(y, off, d)
os.truncate(m("g7"), 3 * CS + 9500); y = tr(y, 3 * CS + 9500)
os.truncate(b("g7"), 3 * CS + 40000); y = tr(y, 3 * CS + 40000)
d = os.urandom(2000); write_at(m("g7"), 3 * CS + 20000, d); y = put(y, 3 * CS + 20000, d)
os.truncate(m("g7"), 3 * CS + 21000); y = tr(y, 3 * CS + 21000)
os.truncate(b("g7"), 2 * MiB); y = tr(y, 2 * MiB)
same("g7 spans and boundaries before the sweep", b("g7"), y)
wait_swept("g7")
same("g7 after retirement (clips alone mask)", b("g7"), y)
same("g7 after retirement on A", m("g7"), y)
d = os.urandom(1500); write_at(b("g7"), 3 * CS + 21500, d); y = put(y, 3 * CS + 21500, d)
same("g7 rewrite into the clipped chunk after retirement", m("g7"), y)
models["g7"] = y

# g8: packed small files truncated and unlinked.
os.makedirs(m("pk"), exist_ok=True)
small = {}
for i in range(int(os.environ.get("PACK_FILES", "400"))):
    d = os.urandom(rng.randrange(200, 6000)); write_at(m("pk/f%d" % i), 0, d, create=True); small[i] = d
logs = [os.path.join(R, "logs", "efsd-n%d.log" % i) for i in (1, 2, 3)]
def packed_total():
    best = 0
    for lg in logs:
        for mm in re.finditer(r"packed_total=(\d+)", open(lg, errors="replace").read()):
            best = max(best, int(mm.group(1)))
    return best
t0 = time.time()
while time.time() - t0 < 180 and packed_total() < len(small) // 2: time.sleep(2)
pt = packed_total()
check("g8 small files packed", pt >= len(small) // 2, "packed_total=%d after %.0f s" % (pt, time.time() - t0))
stop = threading.Event(); readerr = []
def reader():
    while not stop.is_set():
        i = rng.randrange(len(small) + 1)
        try:
            readall(b("pk/f%d" % i))
        except FileNotFoundError:
            pass
        except OSError as e:
            readerr.append(e.errno)
th = threading.Thread(target=reader); th.start()
for i in range(0, len(small), 5):
    n = [0, 37, len(small[i]) // 2, len(small[i]) + 900][i // 5 % 4]
    os.truncate(m("pk/f%d" % i), n); small[i] = tr(small[i], n)
for i in range(1, max(small) + 1, 9):
    if i in small:
        os.unlink(m("pk/f%d" % i)); small.pop(i)
for i in range(2, max(small) + 1, 11):
    if i in small:
        d = os.urandom(100); write_at(m("pk/f%d" % i), 50, d); small[i] = put(small[i], 50, d)
stop.set(); th.join()
check("g8 reads of the other files during changes: no I/O errors", not readerr, str(readerr[:5]))
wait_swept("g8")
time.sleep(5)
bad = [i for i, d in small.items() if readall(b("pk/f%d" % i)) != d]
check("g8 every packed file correct after sweep and repacking", not bad, "bad=%s" % bad[:10])
for i, d in small.items(): models["pk/f%d" % i] = d
pickle.dump(models, open(state, "wb"))
sys.exit(1 if fails else 0)
EOF
if [ -n "${D25_PROBE:-}" ]; then
  python3 - "$R/mA" <<'PY'
import os, sys
p = os.path.join(sys.argv[1], "probe"); fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
blk = os.urandom(4 << 20)
for k in range(64): os.write(fd, blk)
os.fsync(fd); os.close(fd)
PY
  touch "$R/sweep-hold"; truncate -s 0 "$R/mA/probe"; sleep 3
  for i in 1 2 3; do "$REL/efs-mgmt" gc-status $A:$((PORT+i)) 2>&1; echo "gcrc=$?"; done
  [ -n "${D25_PROBE_SLEEP:-}" ] && sleep "$D25_PROBE_SLEEP"
  "$REL/efs-mgmt" status $A:$((PORT+1))
  rm -f "$R/sweep-hold"; sleep 8
  for i in 1 2 3; do "$REL/efs-mgmt" gc-status $A:$((PORT+i)) | grep -i "trunc\|usage"; done
  exit 0
fi
ALLOC_GIB=$ALLOC_GIB PACK_FILES=$PACK_FILES python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" hot; rc1=$?
# g6b: a restart of every daemon in the middle of a sweep.
python3 - "$R/mA" <<'PY'
import os, sys
p = os.path.join(sys.argv[1], "mid"); fd = os.open(p, os.O_RDWR | os.O_CREAT, 0o644)
blk = os.urandom(4 << 20)
for k in range(256): os.write(fd, blk)
os.fsync(fd); os.close(fd)
open(os.path.join(os.path.dirname(sys.argv[1]), "sweep-hold"), "w").close()
os.truncate(p, 0)
PY
umount_all; stop_daemons; start_daemons; mount_all
rm -f "$R/sweep-hold"
echo "  -- restarted all daemons with the sweep pending, then released it"
python3 - "$R/mA" "$R/mB" <<'PY' || rc1=1
import os, re, subprocess, sys, time
REL, PORT = os.environ["D25_REL"], int(os.environ["D25_PORT"])
def pending():
    n = 0
    for i in (1, 2, 3):
        r = subprocess.run([REL + "/efs-mgmt", "gc-status", "%s:%d" % (os.environ["D25_ADDR"], PORT + i)], capture_output=True, text=True)
        m = re.search(r"trunc_pending group0=(\d+) group2=(\d+)", r.stdout)
        if r.returncode or not m:
            raise SystemExit("gc-status unreadable on node %d" % i)
        n = max(n, int(m.group(1)) + int(m.group(2)))
    return n
t0 = time.time()
first = pending()
print("  %-5s g6 sweep still pending after the restart (%d inodes)" % ("PASS" if first else "FAIL", first))
while time.time() - t0 < 600 and pending(): time.sleep(2)
ok = first > 0 and pending() == 0 and os.stat(os.path.join(sys.argv[2], "mid")).st_size == 0
print("  %-5s g6 sweep completes after a restart mid-sweep (%.0f s)" % ("PASS" if ok else "FAIL", time.time() - t0))
sys.exit(0 if ok else 1)
PY
umount_all; mount_all
python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" cold; rc2=$?
umount_all; stop_daemons; start_daemons; mount_all
echo "  -- after an orderly restart of all daemons"
python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" cold || rc2=1
echo "  logs: truncate-apply-errors=$(cat "$R"/logs/efsd-n*.log | grep -c 'apply truncate-lazy rc')" \
     "sweep-errors=$(cat "$R"/logs/efsd-n*.log | grep -c 'apply trunc-sweep rc')" \
     "bootstrap-refusals=$(cat "$R"/logs/efsd-n*.log | grep -c 'apply bootstrap-lane rc')" \
     "pub-reject=$(cat "$R"/logs/efsd-n*.log | grep -c 'pub-reject')" \
     "publish-stalled=$(cat "$R"/logs/fuse-*.log 2>/dev/null | grep -c 'publish-stalled')" \
     "eio=$(cat "$R"/logs/fuse-*.log 2>/dev/null | grep -ci 'EIO')"
umount_all
[ $rc1 = 0 ] && [ $rc2 = 0 ] && [ $UNRESOLVED = 0 ] && echo "== $L VERDICT PASS" || echo "== $L VERDICT FAIL"
