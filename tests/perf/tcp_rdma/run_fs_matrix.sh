#!/bin/bash
# Private TCP/RDMA filesystem matrix. Does not touch port 19810.
#
# Three efsd processes on fcstor007 (ports 19970–19972, storage under
# /tmp/efs-bench-priv). Two clients: fcstor008 writes, fcstor007 reads,
# so the read cannot be answered from the writer's cache. Stream and
# metadata run at the same time on those two hosts. Five repeats, TCP
# then RDMA, servers restarted onto the transport under test.
set -euo pipefail

SSH=$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh
SRV=fcstor007.ib
WRITER=fcstor008.ib
READER=fcstor007.ib
ADDR=172.16.223.61
PORT=19970
EXPORT=efs-bench-priv
WORK=/tmp/efs-bench-priv
MNT=/tmp/efs-bench-mnt
OUT=${1:-$HOME/git/efs/results/measure/$(date -u +%Y%m%d-%H%M)-fs-private}
REPEATS=${EFS_FS_REPEATS:-5}

on() {
    local to=$1 host=$2
    shift 2
    EFS_SSH_TIMEOUT=$to "$SSH" "$host" "$@"
}

# Only processes whose argv names this private port or directory.
kill_private() {
    local host=$1
    on 20 "$host" "python3 - << 'PY'
import os, signal
need = ('--port 19970', '--port 19971', '--port 19972',
        '19970', 'efs-bench-priv', 'efs-bench-mnt')
for pid in os.listdir('/proc'):
    if not pid.isdigit():
        continue
    try:
        comm = open('/proc/%s/comm' % pid).read().strip()
        cmd = open('/proc/%s/cmdline' % pid, 'rb').read().replace(b'\\0', b' ').decode('utf-8', 'replace')
    except OSError:
        continue
    if comm not in ('efsd', 'efs-fuse'):
        continue
    if any(n in cmd for n in need):
        os.kill(int(pid), 9)
PY
timeout 3 fusermount3 -uz $MNT >/dev/null 2>&1 || true"
}

cleanup() {
    kill_private "$SRV" || true
    kill_private "$WRITER" || true
}
trap cleanup EXIT

mkdir -p "$OUT"
echo "OUT=$OUT"

# Build on the nodes. make clean drops a stale object from another ISA.
build_one() {
    local host=$1 bins=$2
    on 180 "$host" "rm -rf /tmp/efs && mkdir -p /tmp/efs && rsync -a --delete --exclude='/mnt/' --exclude='*.log' --exclude='/results/' \"\$HOME/git/efs/\" /tmp/efs/ && cd /tmp/efs && make clean >/dev/null && make -j\"\$(nproc)\" $bins && fstype=\$(findmnt -n -o FSTYPE /tmp || echo missing) && echo BUILD_OK host=\$(hostname -s) fstype=\$fstype"
}

echo "building"
if [ "${EFS_FS_SKIP_BUILD:-0}" != 1 ]; then
build_one "$SRV" "efsd efs-fuse efs-mgmt" &
bp1=$!
build_one "$WRITER" "efs-fuse" &
bp2=$!
wait "$bp1"
wait "$bp2"
else
    echo "BUILD_SKIPPED"
fi

DIO=$(on 15 "$SRV" "fstype=\$(findmnt -n -o FSTYPE /tmp || true); if [ \"\$fstype\" = tmpfs ]; then echo; else echo --direct-io; fi")
echo "dio_flag='${DIO}'"

cleanup
on 20 "$SRV" "rm -rf $WORK && mkdir -p $WORK/s1 $WORK/s2 $WORK/s3 $MNT"
on 20 "$WRITER" "rm -rf $WORK && mkdir -p $WORK $MNT"

start_servers() {
    local xport=$1 joined=$2
    local id port join
    for id in 1 2 3; do
        port=$((PORT + id - 1))
        join=""
        if [ "$joined" != 1 ] && [ "$id" != 1 ]; then
            join="--join ${ADDR}:${PORT}"
        fi
        on 20 "$SRV" "cd /tmp/efs && EFS_MD_RAFT_N=3 EFS_TRANSPORT=$xport setsid -f ./efsd --node-id $id --addr $ADDR --port $port --storage $WORK/s$id --quota 40G $DIO $join >$WORK/s$id.log 2>&1 </dev/null && echo started_id=$id"
    done
    local id
    for id in 1 2 3; do
        local ok=0 i
        for i in $(seq 1 40); do
            if on 15 "$SRV" "grep -q listening $WORK/s$id.log"; then
                ok=1
                break
            fi
            sleep 0.2
        done
        if [ "$ok" != 1 ]; then
            echo "server $id did not listen"
            on 15 "$SRV" "tail -30 $WORK/s$id.log" || true
            exit 1
        fi
    done
}

wait_leader() {
    local i
    for i in $(seq 1 40); do
        if on 15 "$SRV" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDR}:${PORT}" | grep -q 'role=LEADER'; then
            return 0
        fi
        sleep 0.3
    done
    echo "no raft leader"
    on 15 "$SRV" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDR}:${PORT}" || true
    exit 1
}

echo "mkfs"
start_servers tcp 0
sleep 0.5
mk=1
for _ in $(seq 1 20); do
    if on 20 "$SRV" "cd /tmp/efs && ./efs-mgmt raft-mkfs ${ADDR}:${PORT} $EXPORT"; then
        mk=0
        break
    fi
    sleep 0.4
done
echo MKFS_RC=$mk
if [ "$mk" != 0 ]; then
    on 15 "$SRV" "tail -40 $WORK/s1.log" || true
    exit 1
fi
wait_leader

mount_one() {
    local host=$1 xport=$2
    on 20 "$host" "mkdir -p $MNT $WORK; rm -f $WORK/fuse.log; cd /tmp/efs && EFS_TRANSPORT=$xport setsid -f ./efs-fuse ${ADDR}:${PORT} $EXPORT $MNT >$WORK/fuse.log 2>&1 </dev/null && echo mounted"
    local i ok=0
    for i in $(seq 1 50); do
        if on 15 "$host" "findmnt -n -o FSTYPE $MNT | grep -qx fuse.efs-fuse && stat $MNT >/dev/null"; then
            ok=1
            break
        fi
        sleep 0.2
    done
    if [ "$ok" != 1 ]; then
        echo "mount failed on $host ($xport)"
        on 15 "$host" "cat $WORK/fuse.log" || true
        exit 1
    fi
}

fuse_pid() {
    local host=$1
    on 15 "$host" "python3 - << 'PY'
import os
for pid in os.listdir('/proc'):
    if not pid.isdigit():
        continue
    try:
        comm = open('/proc/%s/comm' % pid).read().strip()
        cmd = open('/proc/%s/cmdline' % pid, 'rb').read().replace(b'\\0', b' ').decode()
    except OSError:
        continue
    if comm == 'efs-fuse' and '19970' in cmd and 'efs-bench-priv' in cmd:
        print(pid)
        break
PY"
}

unmount_both() {
    kill_private "$WRITER" || true
    # reader is the server host; kill_private would also kill efsd.
    on 20 "$READER" "python3 - << 'PY'
import os, signal
for pid in os.listdir('/proc'):
    if not pid.isdigit():
        continue
    try:
        comm = open('/proc/%s/comm' % pid).read().strip()
        cmd = open('/proc/%s/cmdline' % pid, 'rb').read().replace(b'\\0', b' ').decode()
    except OSError:
        continue
    if comm == 'efs-fuse' and '19970' in cmd:
        os.kill(int(pid), 9)
PY
timeout 3 fusermount3 -uz $MNT >/dev/null 2>&1 || true"
}

switch_transport() {
    local xport=$1
    unmount_both
    on 20 "$SRV" "python3 - << 'PY'
import os, signal
for pid in os.listdir('/proc'):
    if not pid.isdigit():
        continue
    try:
        comm = open('/proc/%s/comm' % pid).read().strip()
        cmd = open('/proc/%s/cmdline' % pid, 'rb').read().replace(b'\\0', b' ').decode()
    except OSError:
        continue
    if comm == 'efsd' and ('19970' in cmd or '19971' in cmd or '19972' in cmd):
        os.kill(int(pid), 9)
PY"
    sleep 0.5
    start_servers "$xport" 1
    wait_leader
    mount_one "$WRITER" "$xport"
    mount_one "$READER" "$xport"
}

run_phase() {
    local host=$1 xport=$2 phase=$3 dir=$4
    shift 4
    local pid
    pid=$(fuse_pid "$host" | tr -d '[:space:]')
    if [ -z "$pid" ]; then
        echo "no fuse pid on $host"
        exit 1
    fi
    on 180 "$host" "EFS_BENCH_TRANSPORT=$xport EFS_BENCH_MOUNT=$MNT EFS_BENCH_FUSE_LOG=$WORK/fuse.log EFS_BENCH_FUSE_PID=$pid $* bash \$HOME/git/efs/tests/perf/tcp_rdma/run_fs.sh $phase $dir"
}

FAIL=0
# Servers are already TCP from mkfs.
for rep in $(seq 1 "$REPEATS"); do
    for xport in tcp rdma; do
        echo "=== rep $rep $xport ==="
        dir=$OUT/r${rep}-${xport}
        mkdir -p "$dir"
        if [ "$rep" = 1 ] && [ "$xport" = tcp ]; then
            mount_one "$WRITER" tcp
            mount_one "$READER" tcp
        else
            switch_transport "$xport"
        fi
        wpid=$(fuse_pid "$WRITER" | tr -d '[:space:]')
        rpid=$(fuse_pid "$READER" | tr -d '[:space:]')
        echo "pids writer=$wpid reader=$rpid"
        # Overlapping windows: stream on the writer host, metadata on the reader.
        set +e
        on 180 "$WRITER" "EFS_BENCH_TRANSPORT=$xport EFS_BENCH_MOUNT=$MNT EFS_BENCH_FUSE_LOG=$WORK/fuse.log EFS_BENCH_FUSE_PID=$wpid bash \$HOME/git/efs/tests/perf/tcp_rdma/run_fs.sh stream $dir" \
            >"$dir/stream.out" 2>"$dir/stream.err" &
        sp=$!
        on 180 "$READER" "EFS_BENCH_TRANSPORT=$xport EFS_BENCH_MOUNT=$MNT EFS_BENCH_FUSE_LOG=$WORK/fuse.log EFS_BENCH_FUSE_PID=$rpid bash \$HOME/git/efs/tests/perf/tcp_rdma/run_fs.sh meta $dir" \
            >"$dir/meta.out" 2>"$dir/meta.err" &
        mp=$!
        wait "$sp" || FAIL=1
        wait "$mp" || FAIL=1
        run_phase "$WRITER" "$xport" cold-write "$dir" || FAIL=1
        name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$dir/cold-write.json")
        sha=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sha256"])' "$dir/cold-write.json")
        whost=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["writer_host"])' "$dir/cold-write.json")
        on 60 "$READER" "EFS_BENCH_TRANSPORT=$xport EFS_BENCH_MOUNT=$MNT EFS_BENCH_FUSE_LOG=$WORK/fuse.log EFS_BENCH_FUSE_PID=$rpid EFS_BENCH_PATH=$MNT/$name EFS_BENCH_WRITER_HOST=$whost EFS_BENCH_EXPECT_SHA=$sha bash \$HOME/git/efs/tests/perf/tcp_rdma/run_fs.sh cold-read $dir" \
            >"$dir/cold-read.out" 2>"$dir/cold-read.err" || FAIL=1
        on 120 "$READER" "EFS_BENCH_TRANSPORT=$xport EFS_BENCH_MOUNT=$MNT EFS_BENCH_FUSE_LOG=$WORK/fuse.log EFS_BENCH_FUSE_PID=$rpid EFS_BENCH_PATH=$MNT/$name bash \$HOME/git/efs/tests/perf/tcp_rdma/run_fs.sh idle $dir" \
            >"$dir/idle.out" 2>"$dir/idle.err" || FAIL=1
        EFS_BENCH_TRANSPORT=$xport EFS_BENCH_STREAM_JSON=$dir/stream.json EFS_BENCH_META_JSON=$dir/meta.json \
            bash "$HOME/git/efs/tests/perf/tcp_rdma/run_fs.sh" overlap "$dir" \
            >"$dir/overlap.out" 2>"$dir/overlap.err" || FAIL=1
        set -e
        echo "phase_fail_so_far=$FAIL"
    done
done

python3 - "$OUT" << 'PY'
import json, os, sys
out = sys.argv[1]
print("transport repeat stream_mib_s create_med_us stat_med_us rename_med_us cold_read_mib_s idle0_us idle1000_us overlap")
for rep in range(1, 6):
    for xp in ("tcp", "rdma"):
        d = os.path.join(out, "r%d-%s" % (rep, xp))
        def load(name):
            p = os.path.join(d, name)
            if not os.path.isfile(p):
                return {}
            return json.load(open(p))
        st, meta, cr, ov = load("stream.json"), load("meta.json"), load("cold-read.json"), load("overlap.json")
        idle = []
        ip = os.path.join(d, "idle.jsonl")
        if os.path.isfile(ip):
            idle = [json.loads(l) for l in open(ip) if l.strip()]
        def ius(ms):
            for r in idle:
                if r.get("idle_ms") == ms:
                    return r.get("median_us")
            return None
        gib = st.get("gib_s")
        mib = (gib * 1024.0) if isinstance(gib, (int, float)) else None
        rg = cr.get("gib_s")
        rmib = (rg * 1024.0) if isinstance(rg, (int, float)) else None
        def f(v):
            return "-" if v is None else ("%.1f" % v if isinstance(v, float) else str(v))
        print(xp, rep, f(mib), f(meta.get("create_median_us")), f(meta.get("stat_median_us")),
              f(meta.get("rename_median_us")), f(rmib), f(ius(0)), f(ius(1000)), ov.get("status"))
PY

if [ "$FAIL" != 0 ]; then
    echo "filesystem matrix recorded failures in $OUT"
    exit 1
fi
echo "wrote $OUT"
