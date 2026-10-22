#!/bin/bash
# d25_step3_gates.sh RELEASE_DIR LABEL
#
# D25 step 3 gates g0-g4 for the single-entry logical truncate, on a private
# 3-daemon cluster on one host with two FUSE mounts A and B (ports
# PORT+1..PORT+3, default 17671-17673; stores under ${D25_ROOT:-/tmp/d25-gates}
# /<label>, --direct-io, 64 GiB quota). Refuses to start when a port is in use;
# never touches orcd-mini (17432-17434).
#
# LAZY=1 (default) exports EFS_D25_LAZY_TRUNCATE=1 to the daemons and the
# clients. g0 and g2 need a build with EFS_FAULTS=1 (fault files under the
# label directory arm the hooks in host_truncate_lazy). LAZY=0 runs only g1
# on the eager path, for the timing baseline.
#
#   g0 300 MiB i%256 pattern; definite refusal before propose and at apply:
#      truncate fails, whole file verifies on A and B; commit with the reply
#      lost: the same-opid retry returns success, applied once
#   g1 O_TRUNC latency: sparse 100 GiB (64 MiB written in 64 places) and an
#      allocated ALLOC_GIB file; whole-syscall time, file usable afterwards
#   g2 lost reply with an intervening pwrite on B (kept); distinct truncate to
#      the same size truncates again; same-size and zero-size truncates
#   g3 the architecture §7.3 / t7 sequence, then OPS random operations
#      (shrink, extend, pwrite, O_APPEND, truncate 0) alternating A and B,
#      each verified on the other mount against a model
#   g4 buffered writes on B across lanes racing truncates on A, REPS times:
#      A and B agree, no EIO
#   then remount both, restart all daemons (same stores), remount, and compare
#   every modelled file again; grep logs for refusals, stalls and EIO.
# Raw logs stay under the label directory (outside Git); stores are removed
# on exit unless D25_KEEP=1.
set -u
REL=$1; L=$2
PORT=${PORT:-17670}; R=${D25_ROOT:-/tmp/d25-gates}/$L; A=${D25_ADDR:-127.0.0.1}
LAZY=${LAZY:-1}; OPS=${OPS:-300}; REPS=${REPS:-20}; ALLOC_GIB=${ALLOC_GIB:-4}
export EFS_TRANSPORT=${EFS_TRANSPORT:-tcp} EFS_MD_RAFT_N=3 ASAN_OPTIONS=detect_leaks=0
export EFS_D25_LAZY_TRUNCATE=$LAZY
export EFS_FAULT_TRUNCATE_BEFORE_FILE=$R/fault-before EFS_FAULT_TRUNCATE_APPLY_FILE=$R/fault-apply
export EFS_FAULT_TRUNCATE_DROP_REPLY_FILE=$R/fault-drop
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
echo "== $L $("$REL/efsd" --version | head -1) transport=$EFS_TRANSPORT lazy=$LAZY"

cat > "$R/gates.py" <<'EOF'
import errno, hashlib, os, pickle, random, sys, threading, time
mA, mB, R, phase, lazy = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] == "1"
MiB = 1 << 20; CS = 128 * 1024; rng = random.Random(2525)
fails = []
state = os.path.join(R, "models.pkl")
def check(name, ok, detail=""):
    print("  %-5s %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True); ok or fails.append(name)
def h(b): return hashlib.sha256(b).hexdigest()[:16]
def readall(p):
    with open(p, "rb") as f: return f.read()
def write_at(p, off, data, create=False, sync=True):
    fd = os.open(p, os.O_RDWR | (os.O_CREAT if create else 0), 0o644)
    try:
        os.pwrite(fd, data, off)
        if sync: os.fsync(fd)
    finally: os.close(fd)
def same(name, path, model):
    got = readall(path)
    bad = next((i for i in range(min(len(got), len(model))) if got[i] != model[i]), -1) if got != model else -1
    check(name, got == model, "size=%d/%d sha=%s/%s first_diff=%d" % (len(got), len(model), h(got), h(model), bad))
def tr(model, n): return model[:n] + b"\0" * max(0, n - len(model))
def put(model, off, data):
    if off > len(model): model += b"\0" * (off - len(model))
    return model[:off] + data + model[off + len(data):]
def arm(name, text=""):
    with open(os.path.join(R, name), "w") as f: f.write(text)
def disarm(name):
    try: os.unlink(os.path.join(R, name))
    except FileNotFoundError: pass
def in_mnt(m, name): return os.path.join(m, name)
models = {}
if phase == "cold":
    models = pickle.load(open(state, "rb"))
    for name, model in sorted(models.items()):
        same("cold B " + name, in_mnt(mB, name), model)
        same("cold A " + name, in_mnt(mA, name), model)
    sys.exit(1 if fails else 0)

def g1():
    # O_TRUNC latency, whole syscall (client flush + RPC + one entry).
    fa = in_mnt(mA, "big-sparse")
    fd = os.open(fa, os.O_RDWR | os.O_CREAT, 0o644); os.ftruncate(fd, 100 << 30)
    blk = os.urandom(MiB)
    for k in range(64): os.pwrite(fd, blk, k * ((100 << 30) // 64))
    os.fsync(fd); os.close(fd)
    t = time.time(); fd = os.open(fa, os.O_WRONLY | os.O_TRUNC); dt = time.time() - t; os.close(fd)
    check("g1 sparse 100GiB O_TRUNC", os.stat(fa).st_size == 0, "%.3f s" % dt)
    write_at(fa, 0, b"after"); same("g1 sparse usable after", in_mnt(mB, "big-sparse"), b"after")
    fb = in_mnt(mA, "big-alloc"); gib = int(os.environ.get("ALLOC_GIB", "4"))
    fd = os.open(fb, os.O_RDWR | os.O_CREAT, 0o644); blk = os.urandom(4 * MiB)
    for k in range(gib * 256): os.write(fd, blk)
    os.fsync(fd); os.close(fd)
    t = time.time(); fd = os.open(fb, os.O_WRONLY | os.O_TRUNC); dt = time.time() - t; os.close(fd)
    check("g1 allocated %dGiB O_TRUNC" % gib, os.stat(fb).st_size == 0, "%.3f s" % dt)
    write_at(fb, 0, b"after"); same("g1 allocated usable after", in_mnt(mB, "big-alloc"), b"after")
    os.unlink(fa); os.unlink(fb)

if phase == "g1":
    g1(); sys.exit(1 if fails else 0)
if phase == "repro":
    # The g3 op 125..129 shape: B writes over chunk 1, A extends and
    # truncates to 0, B writes a partial chunk 1. Chunk 1 must read zero
    # below the new write.
    pa, pb = in_mnt(mA, "repro"), in_mnt(mB, "repro")
    m = b""; write_at(pb, 0, b"", create=True)
    d = os.urandom(258078); write_at(pb, 59346, d); m = put(m, 59346, d)
    os.truncate(pa, 1911116); m = tr(m, 1911116)
    os.truncate(pa, 0); m = b""
    print("  repro: wrote, extended, truncated to 0", flush=True)
    d = os.urandom(288394); write_at(pb, 139501, d); m = put(m, 139501, d)
    for label, p in (("A", pa), ("B", pb)):
        got = readall(p)
        bad = next((k for k in range(min(len(got), len(m))) if got[k] != m[k]), -1)
        seg = got[bad:bad + 64] if bad >= 0 else b""
        print("    %s: size=%d first_diff=%d zeros=%s" % (label, len(got), bad,
              bool(seg) and seg == b"\0" * len(seg)), flush=True)
    time.sleep(1.0)
    same("repro partial write after peer truncate-0 (A)", pa, m)
    same("repro partial write after peer truncate-0 (B)", pb, m)
    sys.exit(1 if fails else 0)

# g0: refusal atomicity and an ambiguous (lost-reply) outcome.
pat = bytes(range(256)) * (300 * MiB // 256)
fa, fb = in_mnt(mA, "w123"), in_mnt(mB, "w123")
write_at(fa, 0, pat, create=True)
for hook in ("fault-before", "fault-apply"):
    arm(hook)
    t = time.time(); err = None
    try: os.truncate(fa, 0)
    except OSError as e: err = e.errno
    disarm(hook)
    check("g0 %s: truncate refused" % hook, err is not None, "errno=%s after %.1f s" % (err, time.time() - t))
    same("g0 %s: whole file on B" % hook, fb, pat)
    same("g0 %s: whole file on A" % hook, fa, pat)
arm("fault-drop", "0")
os.truncate(fa, 1000123)
m = tr(pat, 1000123)
check("g0 lost reply resolved by same-opid retry", not os.path.exists(os.path.join(R, "fault-drop")))
same("g0 lost reply: applied once, prefix on B", fb, m)
models["w123"] = m

# g2: lost reply with an intervening pwrite on B.
ga, gb = in_mnt(mA, "g2"), in_mnt(mB, "g2")
g = os.urandom(4 * MiB); write_at(ga, 0, g, create=True)
arm("fault-drop", "3000")
res = {}
def a_trunc():
    try: os.truncate(ga, 500000); res["rc"] = 0
    except OSError as e: res["rc"] = e.errno
th = threading.Thread(target=a_trunc); th.start()
deadline = time.time() + 20
while time.time() < deadline and os.stat(gb).st_size != 500000: time.sleep(0.05)
d = os.urandom(4096); write_at(gb, 600000, d)
th.join()
g = put(tr(g, 500000), 600000, d)
check("g2 lost-reply truncate returns success", res.get("rc") == 0, str(res))
same("g2 intervening pwrite kept (A)", ga, g)
same("g2 intervening pwrite kept (B)", gb, g)
os.truncate(ga, 500000); g = tr(g, 500000)
same("g2 distinct same-size truncate truncates again", gb, g)
os.truncate(ga, 500000); same("g2 same-size truncate no-op", gb, g)
os.truncate(gb, 0); g = b""; same("g2 zero-size truncate", ga, g)
models["g2"] = g

# g3: the §7.3 / t7 sequence, then a random cross-mount sequence.
sa, sb = in_mnt(mA, "t7"), in_mnt(mB, "t7")
s = os.urandom(MiB); write_at(sa, 0, s, create=True)
os.truncate(sa, 100); s = tr(s, 100)
os.truncate(sb, MiB); s = tr(s, MiB)
same("g3 t7 shrink 100, extend 1MiB: zeros past 100", sa, s)
d = os.urandom(100); write_at(sa, 500, d); s = put(s, 500, d)
os.truncate(sb, 700); s = tr(s, 700)
os.truncate(sa, 2 * MiB); s = tr(s, 2 * MiB)
same("g3 t7 [0,100) old, [100,500) 0, [500,600) new, [600,700) 0, rest 0", sb, s)
models["t7"] = s
ra, rb = in_mnt(mA, "rand"), in_mnt(mB, "rand")
r = b""; write_at(ra, 0, b"", create=True)
busy = 0; kinds = {}; trace = []; history = [b""]
for i in range(int(os.environ.get("OPS", "300"))):
    me, other = (ra, rb) if i % 2 == 0 else (rb, ra)
    op = rng.choice(["shrink", "shrink", "extend", "write", "write", "write", "append", "zero"])
    kinds[op] = kinds.get(op, 0) + 1
    try:
        if op == "shrink" and r:
            n = rng.randrange(len(r)); os.truncate(me, n); r = tr(r, n); trace.append((i, op, me[-6:], n))
        elif op == "extend":
            n = len(r) + rng.randrange(1, 3 * MiB); os.truncate(me, n); r = tr(r, n); trace.append((i, op, me[-6:], n))
        elif op == "write":
            off = rng.randrange(0, max(1, len(r) + 2 * CS)); d = os.urandom(rng.randrange(1, 300 * 1024))
            write_at(me, off, d); r = put(r, off, d); trace.append((i, op, me[-6:], off, len(d)))
        elif op == "append":
            d = os.urandom(rng.randrange(1, 200 * 1024))
            fd = os.open(me, os.O_WRONLY | os.O_APPEND); os.write(fd, d); os.fsync(fd); os.close(fd)
            trace.append((i, op, me[-6:], len(r), len(d))); r = r + d
        elif op == "zero":
            os.truncate(me, 0); r = b""; trace.append((i, op, me[-6:]))
    except OSError as e:
        if e.errno not in (errno.EBUSY, errno.EAGAIN): raise
        busy += 1  # a full fence history (no sweep before step 4): refused, nothing changed
    if len(r) > 24 * MiB: os.truncate(me, 0); r = b""
    history.append(r)
    got = readall(other)
    if got != r:
        same("g3 random op %d (%s) on other mount" % (i, op), other, r)
        bad = next(k for k in range(min(len(got), len(r))) if got[k] != r[k])
        end = next((k for k in range(bad, min(len(got), len(r))) if got[k] == r[k]), min(len(got), len(r)))
        seg = got[bad:end]
        src = [j for j, hm in enumerate(history[:-1]) if hm[bad:end] == seg and len(hm) >= end]
        print("    bad=[%d,%d) zeros=%s matches_history_states=%s" % (bad, end, seg == b"\0" * len(seg), src[-5:]))
        for t in trace[-12:]: print("    trace", t)
        break
else:
    check("g3 %s random ops verified on the other mount" % os.environ.get("OPS", "300"), True,
          "kinds=%s history-busy=%d" % (kinds, busy))
models["rand"] = r

# g4: buffered writes on B across lanes racing truncates on A.
ca, cb = in_mnt(mA, "conc"), in_mnt(mB, "conc")
write_at(ca, 0, os.urandom(16 * MiB), create=True)
eio = 0
for rep in range(int(os.environ.get("REPS", "20"))):
    stop = threading.Event()
    def b_writer():
        global eio
        try:
            fd = os.open(cb, os.O_WRONLY)
            while not stop.is_set():
                ci = rng.randrange(128); os.pwrite(fd, os.urandom(4096), ci * CS + 1000)
            os.close(fd)
        except OSError as e:
            eio += e.errno == errno.EIO
    th = threading.Thread(target=b_writer); th.start()
    for k in range(3):
        try: os.truncate(ca, rng.randrange(16 * MiB))
        except OSError as e:
            eio += e.errno == errno.EIO
    stop.set(); th.join()
    xa, xb = readall(ca), readall(cb)
    if xa != xb:
        check("g4 rep %d: A and B agree" % rep, False, "A=%d/%s B=%d/%s" % (len(xa), h(xa), len(xb), h(xb))); break
else:
    check("g4 %s racing truncate/buffered-write rounds: A==B" % os.environ.get("REPS", "20"), True)
check("g4 no EIO", eio == 0, "eio=%d" % eio)
pickle.dump(models, open(state, "wb"))
sys.exit(1 if fails else 0)
EOF
if [ -n "${D25_ONLY:-}" ]; then
  python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" "$D25_ONLY" "$LAZY"; rc1=$?; rc2=0
elif [ "$LAZY" = 1 ]; then
  python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" hot 1; rc1=$?
  ALLOC_GIB=$ALLOC_GIB python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" g1 1 || rc1=1
  umount_all; mount_all
  python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" cold 1; rc2=$?
  umount_all; stop_daemons; start_daemons; mount_all
  echo "  -- after an orderly restart of all daemons"
  python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" cold 1 || rc2=1
else
  ALLOC_GIB=$ALLOC_GIB python3 "$R/gates.py" "$R/mA" "$R/mB" "$R" g1 0; rc1=$?; rc2=0
fi
echo "  logs: truncate-apply-errors=$(cat "$R"/logs/efsd-n*.log | grep -c 'apply truncate-lazy rc')" \
     "faults=$(cat "$R"/logs/efsd-n*.log | grep -c 'reply dropped')" \
     "bootstrap-refusals=$(cat "$R"/logs/efsd-n*.log | grep -c 'apply bootstrap-lane rc')" \
     "pub-reject=$(cat "$R"/logs/efsd-n*.log | grep -c 'pub-reject')" \
     "publish-stalled=$(cat "$R"/logs/fuse-*.log 2>/dev/null | grep -c 'publish-stalled')" \
     "eio=$(cat "$R"/logs/fuse-*.log 2>/dev/null | grep -ci 'EIO')"
umount_all
[ $rc1 = 0 ] && [ $rc2 = 0 ] && [ $UNRESOLVED = 0 ] && echo "== $L VERDICT PASS" || echo "== $L VERDICT FAIL"
