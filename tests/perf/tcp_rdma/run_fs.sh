#!/bin/bash
# Filesystem TCP/RDMA measurements. Separate from tests/perf/tcp_rdma/xprt_bench.
#
# This script does not start efsd, does not remount, and does not change
# EFS_TRANSPORT. It refuses a mount whose command line is the live cluster
# (port 19810 or 19820). Point it at a private export the operator already
# brought up with an explicit transport.
#
# A stat or create here is a filesystem RPC (metadata includes a Raft
# commit). A stream with conv=fsync is a durable write. Neither number is
# the transport-echo latency from xprt_bench.
set -euo pipefail

MOUNT=${EFS_BENCH_MOUNT:-/tmp/efs-mount}
FUSE_LOG=${EFS_BENCH_FUSE_LOG:-/tmp/efs/fuse.log}
WANT=${EFS_BENCH_TRANSPORT:-}
FUSE_PID=${EFS_BENCH_FUSE_PID:-}

usage() {
    cat >&2 <<EOF
usage: $0 verify|stream|meta|cold-write|cold-read|idle|overlap OUTDIR

Environment:
  EFS_BENCH_TRANSPORT   tcp or rdma (required; auto is rejected)
  EFS_BENCH_MOUNT       FUSE mount (default /tmp/efs-mount)
  EFS_BENCH_FUSE_LOG    this mount's fuse log, truncated at start
  EFS_BENCH_FUSE_PID    required when more than one efs-fuse is running
  EFS_BENCH_WRITER_HOST cold-read refuses to run on the writer host
  EFS_BENCH_EXPECT_SHA  sha256 cold-read must match
  EFS_BENCH_PATH        file path for cold-read / idle
EOF
    exit 2
}

die() {
    echo "fs bench: $*" >&2
    exit 2
}

[[ $# -ge 2 ]] || usage
CMD=$1
OUT=$2
mkdir -p "$OUT"

case "$WANT" in
    tcp|rdma) ;;
    *) die "set EFS_BENCH_TRANSPORT=tcp or rdma (not auto, not empty)" ;;
esac

fuse_pid() {
    if [[ -n "$FUSE_PID" ]]; then
        echo "$FUSE_PID"
        return
    fi
    local n
    n=$(pgrep -x efs-fuse | wc -l) || n=0
    if [[ "$n" -ne 1 ]]; then
        die "expected one efs-fuse, found $n; set EFS_BENCH_FUSE_PID"
    fi
    pgrep -x efs-fuse
}

verify_transport() {
    local pid cmd envt fstype
    pid=$(fuse_pid)
    [[ -d /proc/$pid ]] || die "efs-fuse pid $pid is not running"
    cmd=$(tr '\0' ' ' </proc/"$pid"/cmdline)
    case "$cmd" in
        *:19810*|*:19820*)
            die "refusing live cluster mount ($cmd)"
            ;;
    esac
    envt=$(tr '\0' '\n' </proc/"$pid"/environ | sed -n 's/^EFS_TRANSPORT=//p' | head -1)
    [[ "$envt" == "$WANT" ]] || die "efs-fuse EFS_TRANSPORT=${envt:-unset} want $WANT"
    [[ -f $FUSE_LOG ]] || die "fuse log $FUSE_LOG is missing; pass the log from this mount"
    if [[ "$WANT" == rdma ]]; then
        grep -q "RDMA transport up" "$FUSE_LOG" ||
            die "RDMA requested but $FUSE_LOG has no 'RDMA transport up'"
    else
        if grep -q "RDMA transport up" "$FUSE_LOG"; then
            die "TCP requested but $FUSE_LOG shows RDMA transport up (stale log or wrong transport)"
        fi
    fi
    fstype=$(findmnt -n -o FSTYPE "$MOUNT" || true)
    [[ "$fstype" == "fuse.efs-fuse" ]] || die "findmnt $MOUNT is '${fstype:-missing}', want fuse.efs-fuse"
    stat "$MOUNT" >/dev/null || die "stat $MOUNT failed"
    python3 - "$OUT/verify.json" "$WANT" "$pid" "$cmd" "$FUSE_LOG" "$MOUNT" <<'PY'
import json, sys
row = {
    "role": "fs_verify",
    "status": "ok",
    "transport": sys.argv[2],
    "fuse_pid": int(sys.argv[3]),
    "cmdline": sys.argv[4],
    "fuse_log": sys.argv[5],
    "mount": sys.argv[6],
    "completion": "transport_check",
}
open(sys.argv[1], "w").write(json.dumps(row, indent=2) + "\n")
print(json.dumps(row))
PY
}

case "$CMD" in
    verify)
        verify_transport
        ;;
    stream)
        verify_transport
        python3 - "$MOUNT" "$OUT" <<'PY'
import hashlib, json, os, socket, subprocess, time, sys
mnt, out = sys.argv[1], sys.argv[2]
# A 1 MiB if= file with count=256 makes dd stop at EOF and exit 0,
# having written 1 MiB. The clock must cover a real 256 MiB source.
blob = bytes((i % 251) + 1 for i in range(1024 * 1024))
nbytes = 256 * 1024 * 1024
pat = "/tmp/efs-bench-pattern-256m.bin"
if not os.path.exists(pat) or os.path.getsize(pat) != nbytes:
    with open(pat, "wb") as f:
        for _ in range(256):
            f.write(blob)
digest = hashlib.sha256(blob).hexdigest()
dest = os.path.join(mnt, "efs-bench-stream-%s-%d" % (socket.gethostname(), os.getpid()))
wall0 = time.time()
t0 = time.monotonic()
rc = subprocess.call(["dd", "if=" + pat, "of=" + dest, "bs=1M", "conv=fsync",
                      "status=none"])
elapsed = time.monotonic() - t0
wall1 = time.time()
try:
    got = os.stat(dest).st_size
except OSError:
    got = 0
ok = rc == 0 and got == nbytes
row = {
    "role": "fs_stream",
    "status": "ok" if ok else "short_write",
    "path": dest,
    "bytes": got,
    "elapsed_s": elapsed,
    "gib_s": (got / (1024.0 ** 3) / elapsed) if ok and elapsed > 0 else None,
    "sha256_source": digest,
    "dd_rc": rc,
    "transport": os.environ.get("EFS_BENCH_TRANSPORT"),
    "started_unix": wall0,
    "ended_unix": wall1,
    "completion": "fsync",
    "note": "conv=fsync, not time_based. Source is 256 MiB, a repeated non-zero 1 MiB block. bytes is st_size after fsync. This is a durable write, not a transport echo.",
}
open(os.path.join(out, "stream.json"), "w").write(json.dumps(row, indent=2) + "\n")
print(json.dumps(row))
sys.exit(0 if ok else 1)
PY
        ;;
    meta)
        verify_transport
        python3 - "$MOUNT" "$OUT" <<'PY'
import json, os, socket, time, sys
mnt, out = sys.argv[1], sys.argv[2]
root = os.path.join(mnt, "efs-bench-meta-%d-%s" % (os.getpid(), socket.gethostname()))
os.mkdir(root)
n = int(os.environ.get("EFS_BENCH_META_N", "200"))
samples = []
errors = 0
wall0 = time.time()

def timed(op, fn):
    global errors
    t0 = time.monotonic()
    try:
        fn()
        dt = (time.monotonic() - t0) * 1e6
        samples.append({"op": op, "us": dt, "status": "ok"})
    except OSError as e:
        errors += 1
        samples.append({"op": op, "us": None, "status": "error", "errno": e.errno})

for i in range(n):
    path = os.path.join(root, "f%06d" % i)
    renamed = os.path.join(root, "r%06d" % i)
    timed("create", lambda p=path: os.close(os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)))
    timed("stat", lambda p=path: os.stat(p))
    timed("rename", lambda a=path, b=renamed: os.rename(a, b))

def pct(op, p):
    xs = sorted(s["us"] for s in samples if s["op"] == op and s["us"] is not None)
    if not xs:
        return None
    # nearest rank, same rule as xprt_bench: ceil(p/100 * n) - 1
    import math
    idx = int(math.ceil((p / 100.0) * len(xs))) - 1
    idx = max(0, min(len(xs) - 1, idx))
    return xs[idx]

summary = {
    "role": "fs_meta",
    "status": "ok" if errors == 0 else "error",
    "transport": os.environ.get("EFS_BENCH_TRANSPORT"),
    "n": n,
    "errors": errors,
    "root": root,
    "started_unix": wall0,
    "ended_unix": time.time(),
    "create_median_us": pct("create", 50),
    "create_p99_us": pct("create", 99),
    "stat_median_us": pct("stat", 50),
    "stat_p99_us": pct("stat", 99),
    "rename_median_us": pct("rename", 50),
    "rename_p99_us": pct("rename", 99),
    "completion": "filesystem_metadata",
    "note": "Create/rename include the Raft commit. Not a transport echo. "
            "Run stream on a different client at the same time and check overlap.",
}
open(os.path.join(out, "meta.json"), "w").write(json.dumps(summary, indent=2) + "\n")
open(os.path.join(out, "meta-samples.jsonl"), "w").write(
    "".join(json.dumps(s) + "\n" for s in samples))
print(json.dumps(summary))
sys.exit(0 if errors == 0 else 1)
PY
        ;;
    cold-write)
        verify_transport
        python3 - "$MOUNT" "$OUT" <<'PY'
import hashlib, json, os, socket, subprocess, time, sys
mnt, out = sys.argv[1], sys.argv[2]
name = "efs-bench-cold-%d" % os.getpid()
dest = os.path.join(mnt, name)
blob = bytes((i * 17 + 3) % 251 + 1 for i in range(4 * 1024 * 1024))
src = os.path.join(out, "cold-src.bin")
open(src, "wb").write(blob)
digest = hashlib.sha256(blob).hexdigest()
t0 = time.monotonic()
rc = subprocess.call(["dd", "if=" + src, "of=" + dest, "bs=1M", "conv=fsync",
                      "status=none"])
elapsed = time.monotonic() - t0
row = {
    "role": "fs_cold_write",
    "status": "ok" if rc == 0 else "error",
    "path": dest,
    "name": name,
    "bytes": len(blob) if rc == 0 else 0,
    "sha256": digest,
    "writer_host": socket.gethostname(),
    "elapsed_s": elapsed,
    "dd_rc": rc,
    "transport": os.environ.get("EFS_BENCH_TRANSPORT"),
    "completion": "fsync",
    "note": "Do not read this file back on the writer. The client dcache would answer. "
            "cold-read must run on a different host.",
}
open(os.path.join(out, "cold-write.json"), "w").write(json.dumps(row, indent=2) + "\n")
print(json.dumps(row))
sys.exit(0 if rc == 0 else 1)
PY
        ;;
    cold-read)
        verify_transport
        [[ -n ${EFS_BENCH_PATH:-} ]] || die "set EFS_BENCH_PATH"
        [[ -n ${EFS_BENCH_WRITER_HOST:-} ]] || die "set EFS_BENCH_WRITER_HOST"
        [[ -n ${EFS_BENCH_EXPECT_SHA:-} ]] || die "set EFS_BENCH_EXPECT_SHA"
        this=$(python3 -c 'import socket; print(socket.gethostname())')
        [[ "$this" != "$EFS_BENCH_WRITER_HOST" ]] ||
            die "reader host is the writer ($this); that read hits the client cache"
        python3 - "$OUT" <<'PY'
import hashlib, json, os, socket, time, sys
out = sys.argv[1]
path = os.environ["EFS_BENCH_PATH"]
expect = os.environ["EFS_BENCH_EXPECT_SHA"]
t0 = time.monotonic()
h = hashlib.sha256()
n = 0
err = None
try:
    with open(path, "rb") as f:
        while True:
            b = f.read(1024 * 1024)
            if not b:
                break
            h.update(b)
            n += len(b)
except OSError as e:
    err = e.errno
elapsed = time.monotonic() - t0
digest = h.hexdigest()
ok = err is None and digest == expect and n > 0
row = {
    "role": "fs_cold_read",
    "status": "ok" if ok else ("error" if err is not None else "data_mismatch"),
    "errno": err,
    "path": path,
    "bytes": n,
    "elapsed_s": elapsed,
    "gib_s": (n / (1024.0 ** 3) / elapsed) if elapsed > 0 else None,
    "sha256": digest,
    "expect_sha256": expect,
    "reader_host": socket.gethostname(),
    "writer_host": os.environ["EFS_BENCH_WRITER_HOST"],
    "transport": os.environ.get("EFS_BENCH_TRANSPORT"),
    "completion": "filesystem_read",
    "note": "First open of this path on a different client's mount. Not the writer dcache. Not a transport echo.",
}
open(os.path.join(out, "cold-read.json"), "w").write(json.dumps(row, indent=2) + "\n")
print(json.dumps(row))
sys.exit(0 if ok else 1)
PY
        ;;
    idle)
        verify_transport
        [[ -n ${EFS_BENCH_PATH:-} ]] || die "set EFS_BENCH_PATH to a file that already exists"
        python3 - "$OUT" <<'PY'
import json, math, os, time, sys
out = sys.argv[1]
path = os.environ["EFS_BENCH_PATH"]
gaps = [0, 1, 10, 100, 1000]
n = int(os.environ.get("EFS_BENCH_IDLE_N", "40"))
rows = []
errors = 0
for gap in gaps:
    samples = []
    for i in range(n):
        if gap:
            time.sleep(gap / 1000.0)
        t0 = time.monotonic()
        try:
            os.stat(path)
            samples.append((time.monotonic() - t0) * 1e6)
        except OSError:
            errors += 1
    xs = sorted(samples)

    def pct(p):
        if not xs:
            return None
        idx = int(math.ceil((p / 100.0) * len(xs))) - 1
        return xs[max(0, min(len(xs) - 1, idx))]

    rows.append({
        "role": "fs_idle",
        "transport": os.environ.get("EFS_BENCH_TRANSPORT"),
        "idle_ms": gap,
        "samples": len(xs),
        "median_us": pct(50),
        "p99_us": pct(99),
        "p999_us": pct(99.9),
        "errors": errors,
        "path": path,
        "completion": "filesystem_stat",
        "status": "ok" if xs else "error",
        "note": "Stat after an idle gap. entry/attr timeout is 0, so this should be a server RPC. It is not an fsync and not the SEND echo.",
    })
open(os.path.join(out, "idle.jsonl"), "w").write("".join(json.dumps(r) + "\n" for r in rows))
for r in rows:
    print(json.dumps(r))
sys.exit(0 if errors == 0 else 1)
PY
        ;;
    overlap)
        [[ -f ${EFS_BENCH_STREAM_JSON:-} && -f ${EFS_BENCH_META_JSON:-} ]] ||
            die "set EFS_BENCH_STREAM_JSON and EFS_BENCH_META_JSON"
        python3 - "$OUT" <<'PY'
import json, os, sys
out = sys.argv[1]
stream = json.load(open(os.environ["EFS_BENCH_STREAM_JSON"]))
meta = json.load(open(os.environ["EFS_BENCH_META_JSON"]))
overlap = False
note = "need started_unix and ended_unix on both files"
s0 = stream.get("started_unix")
s1 = stream.get("ended_unix")
m0 = meta.get("started_unix")
m1 = meta.get("ended_unix")
if s0 and s1 and m0 and m1 and s0 <= m1 and m0 <= s1:
    overlap = True
    note = "stream and metadata windows overlap"
row = {
    "role": "fs_overlap",
    "overlap": overlap,
    "stream_transport": stream.get("transport"),
    "meta_transport": meta.get("transport"),
    "same_transport": stream.get("transport") == meta.get("transport"),
    "note": note,
    "completion": "filesystem_metadata",
}
if stream.get("transport") != meta.get("transport"):
    row["status"] = "transport_mismatch"
elif not overlap:
    row["status"] = "no_overlap"
else:
    row["status"] = "ok"
open(os.path.join(out, "overlap.json"), "w").write(json.dumps(row, indent=2) + "\n")
print(json.dumps(row))
sys.exit(0 if row["status"] == "ok" else 1)
PY
        ;;
    *)
        usage
        ;;
esac
