#!/bin/bash
# d25_step2_gates.sh RELEASE_DIR LABEL
#
# D25 step 2 gates g4 (lanes, adoption, BOOTSTRAP) and g5 (views and caches)
# in their step-2 form, on a private 3-daemon cluster on one host with two
# FUSE mounts A and B (ports PORT+1..PORT+3, default 17651-17653; stores
# under ${D25_ROOT:-/tmp/d25-gates}/<label>, --direct-io, 8 GiB quota).
# Refuses to start when a port is in use; never touches orcd-mini
# (17432-17434). Truncate is still the eager path in step 2, so these gates
# check that the inode-authority views, the writer epoch, adoption and the
# BOOTSTRAP floor keep every byte correct across lanes, truncates and
# mounts; lazy-truncate interleavings belong to step 3.
#
# Checks (a model file in memory is the oracle; every read compares bytes):
#   g4.1 48 MiB over all 64 lanes written on A, read on B
#   g4.2 shrink to an unaligned size on A; size and bytes on B
#   g4.3 extend on A; the discarded tail reads zero on B
#   g4.4 pwrites across all lanes and the old boundary; read on B
#   g4.5 truncate to 0, rewrite; read on B
#   g4.6 new sparse file touching every lane (first publish per lane,
#        cross-group lanes need BOOTSTRAP); read on B
#   g5.1 B holds an open, warm descriptor across a truncate+write on A: after
#        the map TTL its fstat keeps the size it had (by design), the path
#        stat shows the new size, and its reads show the new bytes and zeros
#        past the new EOF, never the discarded data
#   g5.2 B reopens: new size and bytes
#   cold remount of both mounts, every file compared again
# Then greps the daemon and client logs for bootstrap/publish refusals,
# stalls and EIO. Raw logs stay under the label directory (outside Git).
set -u
REL=$1; L=$2
PORT=${PORT:-17650}; R=${D25_ROOT:-/tmp/d25-gates}/$L; A=${D25_ADDR:-127.0.0.1}
export EFS_TRANSPORT=${EFS_TRANSPORT:-tcp} ASAN_OPTIONS=detect_leaks=0
for i in 1 2 3; do
  ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "$L: port $((PORT+i)) in use"; exit 1; }
done
rm -rf "$R"; mkdir -p "$R"/{n1,n2,n3,logs,mA,mB}
# D39: the configured member list; the endpoint selects each member and
# efs-mgmt declares as management with it (EFS_CLUSTER_CONF).
CONF="$R/cluster.conf"; : > "$CONF"
for i in 1 2 3; do echo "$i $A:$((PORT+i))" >> "$CONF"; done
export EFS_CLUSTER_CONF="$CONF"
for i in 1 2 3; do
  nohup "$REL/efsd" --cluster "$CONF" --addr "$A" --port $((PORT+i)) --storage "$R/n$i" \
    --quota 8G --direct-io > "$R/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$R/logs/n$i.pid"
  for t in $(seq 60); do grep -q listening "$R/logs/efsd-n$i.log" && break; sleep 0.5; done
  kill -0 "$(cat "$R/logs/n$i.pid")" 2>/dev/null || { echo "$L: efsd n$i exited"; tail -3 "$R/logs/efsd-n$i.log"; exit 1; }
done
umount_all() {
  for m in mA mB; do "$REL/scripts/client.sh" stop "$R/$m" > /dev/null 2>&1; done
}
cleanup() {
  umount_all
  for i in 1 2 3; do kill "$(cat "$R/logs/n$i.pid")" 2>/dev/null; done
  sleep 3; for i in 1 2 3; do kill -9 "$(cat "$R/logs/n$i.pid")" 2>/dev/null; done
  [ "${D25_KEEP:-0}" = 1 ] || rm -rf "$R"/n1 "$R"/n2 "$R"/n3
}
trap cleanup EXIT
for t in $(seq 30); do
  up=$("$REL/efs-mgmt" status $A:$((PORT+1)) 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
  [ "$up" -eq 3 ] && break; sleep 1
done
for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs $A:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
mount_all() {
  for m in mA mB; do
    EFS_FUSE_LOG="$R/logs/fuse-$m.log" "$REL/scripts/client.sh" $A:$((PORT+1)) "$R/$m" >> "$R/logs/client-$m.out" 2>&1
    findmnt -rn "$R/$m" > /dev/null || { echo "$L: mount $m failed"; tail -5 "$R/logs/client-$m.out"; exit 1; }
  done
}
mount_all
echo "== $L $("$REL/efsd" --version | head -1) transport=$EFS_TRANSPORT"

cat > "$R/gates.py" <<'EOF'
import hashlib, os, pickle, random, sys, time
mA, mB, state = sys.argv[1], sys.argv[2], sys.argv[3]
MiB = 1 << 20; CS = 128 * 1024; rng = random.Random(25)
fails = []
def check(name, ok, detail=""):
    print("  %-5s %s %s" % ("PASS" if ok else "FAIL", name, detail)); ok or fails.append(name)
def h(b): return hashlib.sha256(b).hexdigest()[:16]
def readall(p):
    with open(p, "rb") as f: return f.read()
def write_at(p, off, data, create=False):
    fd = os.open(p, os.O_RDWR | (os.O_CREAT if create else 0), 0o644)
    try:
        os.pwrite(fd, data, off); os.fsync(fd)
    finally: os.close(fd)
def same(name, path_b, model):
    got = readall(path_b)
    check(name, got == model, "size=%d sha=%s model=%d/%s" % (len(got), h(got), len(model), h(model)))
def truncate(p, n, model):
    os.truncate(p, n)
    return model[:n] + b"\0" * max(0, n - len(model))
def apply(model, off, data):
    if off > len(model): model += b"\0" * (off - len(model))
    return model[:off] + data + model[off + len(data):]
if sys.argv[4] == "cold":
    models = pickle.load(open(state, "rb"))
    for name, model in sorted(models.items()):
        same("cold " + name, os.path.join(mB, name), model)
        same("cold A " + name, os.path.join(mA, name), model)
    sys.exit(1 if fails else 0)
models = {}
fa, fb = os.path.join(mA, "f"), os.path.join(mB, "f")
m = os.urandom(48 * MiB); m0 = m; write_at(fa, 0, m, create=True)
same("g4.1 48MiB all lanes", fb, m)
m = truncate(fa, 1000123, m)
check("g4.2 size on B", os.stat(fb).st_size == 1000123, str(os.stat(fb).st_size))
same("g4.2 bytes on B", fb, m)
m = truncate(fa, 20 * MiB, m)
old48 = m0[:20 * MiB]
got = readall(fb)
bad = next((i for i in range(len(m)) if i >= len(got) or got[i] != m[i]), -1)
check("g4.3 discarded tail zero", got == m, "size=%d first_diff=%d old_bytes_back=%s" % (
    len(got), bad, bad >= 0 and bad < len(got) and got[bad:bad + 64] == old48[bad:bad + 64]))
offs = [ci * CS + rng.randrange(CS - 4096) for ci in rng.sample(range(160), 64)] + [1000123 - 2048]
for off in offs:
    d = os.urandom(4096); write_at(fa, off, d); m = apply(m, off, d)
same("g4.4 pwrites across lanes and old boundary", fb, m)
m = truncate(fa, 0, m)
d = os.urandom(8 * MiB); write_at(fa, 0, d); m = d
same("g4.5 truncate to 0 and rewrite", fb, m)
models["f"] = m
s = b""
pa, pb = os.path.join(mA, "sparse"), os.path.join(mB, "sparse")
for lane in range(64):
    d = os.urandom(4096); off = (lane + 64 * (lane % 3)) * CS + 512
    write_at(pa, off, d, create=(lane == 0)); s = apply(s, off, d)
same("g4.6 first publish on every lane", pb, s)
models["sparse"] = s
# g5: B holds a warm descriptor across a change on A.
ga, gb = os.path.join(mA, "g"), os.path.join(mB, "g")
g = os.urandom(4 * MiB); write_at(ga, 0, g, create=True)
fd = os.open(gb, os.O_RDONLY)
warm = os.pread(fd, len(g) + 1, 0)
check("g5.0 warm read on B", warm == g)
g = truncate(ga, 300000, g)
d = os.urandom(65536); write_at(ga, 100000, d); g = apply(g, 100000, d)
time.sleep(0.5)  # > EFS_READ_MAP_TTL_MS (200 ms)
st = os.fstat(fd).st_size
held = os.pread(fd, 4 * MiB, 0)
st2 = os.stat(gb).st_size
# An open descriptor keeps this mount's size (efs_fuse_getattr_ino: "Open-fd
# size stays local"); the path stat sees the new size. Bytes through the held
# descriptor must be the new data, and zero past the new EOF: never discarded.
check("g5.1 held fd size local, path stat new", st == 4 * MiB and st2 == 300000,
      "fstat=%d stat(path)=%d" % (st, st2))
check("g5.1 held fd never shows discarded bytes", held == g + b"\0" * (4 * MiB - len(g)),
      "len=%d prefix_new=%s" % (len(held), held[:300000] == g))
os.close(fd)
same("g5.2 reopen on B", gb, g)
models["g"] = g
pickle.dump(models, open(state, "wb"))
sys.exit(1 if fails else 0)
EOF
python3 "$R/gates.py" "$R/mA" "$R/mB" "$R/models.pkl" hot; rc1=$?
umount_all; mount_all
python3 "$R/gates.py" "$R/mA" "$R/mB" "$R/models.pkl" cold; rc2=$?
echo "  logs: bootstrap-refusals=$(cat "$R"/logs/efsd-n*.log | grep -c 'apply bootstrap-lane rc')" \
     "pub-reject=$(cat "$R"/logs/efsd-n*.log | grep -c 'pub-reject')" \
     "publish-stalled=$(cat "$R"/logs/fuse-*.log 2>/dev/null | grep -c 'publish-stalled')" \
     "eio=$(cat "$R"/logs/fuse-*.log 2>/dev/null | grep -ci 'EIO')"
[ $rc1 = 0 ] && [ $rc2 = 0 ] && echo "== $L VERDICT PASS" || echo "== $L VERDICT FAIL"
