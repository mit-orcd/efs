#!/bin/bash
# raft_rotate_stall_ab.sh — measure the metadata stall Raft log rotation
# causes, on a private 3-daemon RDMA cluster on node9901 (ports 17631-17633,
# stores under /tmp). Never touches orcd-mini (ports 17432-17434).
#
#   raft_rotate_stall_ab.sh build SRC SNAP_MIB
#       Copy SRC to SRC-snapSNAP_MIB with EFS_RAFT_SNAP_BYTES set to SNAP_MIB
#       MiB (default 512) so snapshots and rotations happen within minutes,
#       then build efsd, efs-mgmt and efs-fuse there.
#   raft_rotate_stall_ab.sh run BUILD LABEL SECONDS [WRITERS]
#       Start the cluster from BUILD, mount it, run WRITERS (16) threads
#       creating 256-byte files plus one probe that creates, writes and
#       closes a file every 50 ms, for SECONDS. Print the probe's latency
#       summary, its slowest probes, and every rotate/snapshot log line, then
#       stop and delete the cluster. Raw output stays under /tmp/efs-rot/run.
#   raft_rotate_stall_ab.sh analyze LOGDIR
#       Re-run the stall attribution on a finished run's logs directory.
set -u
# Attribute probe stalls: a probe counts toward a window when its interval
# overlaps it. Windows are each node's rotations (end - elapsed .. end) and
# snapshot exports (raft-snap start .. end), unioned across the 3 nodes.
analyze() {
    python3 - "$1/probe.csv" "$1"/efsd-n{1,2,3}.log <<'EOF'
import re, sys, datetime
probes = []
for l in open(sys.argv[1]):
    t, ms, rc = l.split(",")[:3]
    probes.append((float(t), float(ms), int(rc)))
def ts(s):
    return datetime.datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%fZ").replace(
        tzinfo=datetime.timezone.utc).timestamp()
rot, exp = [], []
for n, path in enumerate(sys.argv[2:], 1):
    start = {}
    for l in open(path):
        m = re.match(r"(\S+Z) raft-log: rotate .*elapsed_us=(\d+)", l)
        if m:
            e = ts(m[1]); hold = re.search(r"hold_max_us=(\d+)", l)
            rot.append((e - int(m[2]) / 1e6, e, f"n{n} rotate {m[1]} {int(m[2])/1e3:.0f}ms"
                        + (f" hold_max {int(hold[1])/1e3:.1f}ms" if hold else " (all under d->mu/h->mu)")))
        m = re.match(r"(\S+Z) raft-snap: (start|end) group=(\d+)", l)
        if m and m[2] == "start":
            start[m[3]] = ts(m[1])
        elif m and m[3] in start:
            a = start.pop(m[3])
            exp.append((a, ts(m[1]), f"n{n} export g{m[3]}"))
hit = lambda t, ms, w: t <= w[1] and t + ms / 1e3 >= w[0]
for w in rot:
    print(f"{w[2]}: worst overlapping probe {max([ms for t, ms, rc in probes if hit(t, ms, w)] or [0]):.0f}ms")
cls = {"a rotation": [], "an export, no rotation": [], "neither": []}
for t, ms, rc in probes:
    r = any(hit(t, ms, w) for w in rot); x = any(hit(t, ms, w) for w in exp)
    cls["a rotation" if r else "an export, no rotation" if x else "neither"].append(ms)
for k, v in cls.items():
    v.sort()
    print(f"probes overlapping {k}: n={len(v)} max={v[-1] if v else 0:.0f}ms >1s={sum(1 for x in v if x > 1000)}")
EOF
}

mode=${1:?build|run|analyze}
case $mode in
build)
    src=${2:?src}; mib=${3:?snap MiB}
    dst=$src-snap$mib
    rm -rf "$dst"; cp -a "$src" "$dst"
    sed -i "s|^#define EFS_RAFT_SNAP_BYTES (512ull << 20)|#define EFS_RAFT_SNAP_BYTES (${mib}ull << 20)|" \
        "$dst/include/efs/raft.h"
    grep -q "EFS_RAFT_SNAP_BYTES (${mib}ull << 20)" "$dst/include/efs/raft.h" || { echo "patch failed"; exit 1; }
    id=$(basename "$src")-snap$mib
    cd "$dst" && PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig \
        make -j48 EFS_GIT_ID="$id" efsd efs-mgmt efs-fuse > build.log 2>&1 \
        && echo "built $dst ($id)" || { tail -20 build.log; exit 1; }
    ;;
run)
    rel=${2:?build dir}; label=${3:?label}; secs=${4:?seconds}; writers=${5:-16}
    ab=/tmp/efs-rot/run/$label
    rm -rf "$ab"; mkdir -p "$ab"/{n1,n2,n3,logs,mnt}
    # D39: the configured member list; the endpoint selects each member and
    # efs-mgmt declares as management with it (EFS_CLUSTER_CONF).
    CONF="$ab/cluster.conf"; : > "$CONF"
    for i in 1 2 3; do echo "$i 10.1.99.1:$((17630+i))" >> "$CONF"; done
    export EFS_CLUSTER_CONF="$CONF"
    export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
    export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    for i in 1 2 3; do
        setsid nohup "$rel/efsd" --cluster "$CONF" --addr 10.1.99.1 --port $((17630+i)) \
            --storage "$ab/n$i" --quota 1000G --direct-io \
            > "$ab/logs/efsd-n$i.log" 2>&1 < /dev/null &
        echo $! > "$ab/logs/n$i.pid"
        for t in $(seq 60); do grep -q listening "$ab/logs/efsd-n$i.log" && break; sleep 0.5; done
    done
    for t in $(seq 30); do
        up=$("$rel/efs-mgmt" status 10.1.99.1:17631 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
        [ "$up" -eq 3 ] && break; sleep 1
    done
    for t in $(seq 30); do
        "$rel/efs-mgmt" raft-mkfs 10.1.99.1:17631 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1
    done
    EFS_FUSE_LOG="$ab/logs/fuse.log" "$rel/scripts/client.sh" 10.1.99.1:17631 "$ab/mnt" \
        > "$ab/logs/client.out" 2>&1
    if ! findmnt -rn "$ab/mnt" > /dev/null; then
        echo "$label: mount failed"; tail -5 "$ab/logs/client.out"
    else
        echo "== $label $("$rel/efsd" --version | head -1)"
        python3 - "$ab/mnt" "$secs" "$writers" "$ab/logs/probe.csv" <<'EOF'
import os, sys, threading, time
mnt, secs, writers, out = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
stop = time.time() + secs; blk = b"x" * 256; made = [0] * writers; errs = []
def writer(t):
    n = 0
    while time.time() < stop:
        d = f"{mnt}/w{t}/d{n // 2000}"
        if n % 2000 == 0: os.makedirs(d, exist_ok=True)
        try:
            with open(f"{d}/f{n}", "xb") as f: f.write(blk)
        except OSError as e: errs.append(("writer", time.time(), e.errno))
        n += 1; made[t] = n
os.makedirs(f"{mnt}/probe")
ws = [threading.Thread(target=writer, args=(t,)) for t in range(writers)]
[w.start() for w in ws]
lat = []; k = 0
with open(out, "w") as log:
    while time.time() < stop:
        a = time.time(); rc = 0
        try:
            with open(f"{mnt}/probe/p{k}", "xb") as f: f.write(blk)
            os.stat(f"{mnt}/probe/p{k}")
        except OSError as e: rc = e.errno
        e = time.time() - a; lat.append((e, a, rc)); log.write(f"{a:.3f},{e*1e3:.1f},{rc}\n"); k += 1
        time.sleep(max(0, 0.05 - e))
[w.join() for w in ws]
s = sorted(x[0] for x in lat); n = len(s)
pct = lambda p: s[min(n - 1, int(p * n))] * 1e3
print(f"writes: {sum(made)} files in {secs:.0f}s = {sum(made)/secs:.0f}/s, writer errors {len(errs)}")
print(f"probe: n={n} p50={pct(.5):.1f}ms p99={pct(.99):.1f}ms p99.9={pct(.999):.1f}ms max={s[-1]*1e3:.1f}ms "
      f">1s={sum(1 for x in s if x > 1)} errors={sum(1 for x in lat if x[2])}")
for e, a, rc in sorted(lat, reverse=True)[:5]:
    print(f"  slow probe {time.strftime('%H:%M:%S', time.gmtime(a))}Z {e*1e3:.0f}ms rc={rc}")
for w, a, rc in errs[:5]: print(f"  writer error {time.strftime('%H:%M:%S', time.gmtime(a))}Z errno={rc}")
EOF
        for i in 1 2 3; do
            grep -hE "raft-log: rotate|raft-snap: (start|end|advance)" "$ab/logs/efsd-n$i.log" \
                | sed "s/^/n$i: /"
            printf 'n%s: raft.log bytes now %s\n' "$i" "$(stat -c %s "$ab/n$i/mdraft/log/raft.log")"
        done
        analyze "$ab/logs"
    fi
    "$rel/scripts/client.sh" stop --force-discard "$ab/mnt" > /dev/null 2>&1
    for i in 1 2 3; do kill "$(cat "$ab/logs/n$i.pid")" 2>/dev/null; done
    sleep 3; for i in 1 2 3; do kill -9 "$(cat "$ab/logs/n$i.pid")" 2>/dev/null; done
    rm -rf "$ab"/n1 "$ab"/n2 "$ab"/n3
    ;;
analyze) analyze "${2:?log dir}" ;;
*) echo "usage: $0 build SRC SNAP_MIB | run BUILD LABEL SECONDS [WRITERS] | analyze LOGDIR" >&2; exit 2 ;;
esac
