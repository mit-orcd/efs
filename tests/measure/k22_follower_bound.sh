#!/bin/bash
# K22 / W23: universal follower memory/lag bound on a private 3-daemon
# cluster (one host, TCP, buffered I/O). Run it in a screen on the host.
#
#   bash tests/measure/k22_follower_bound.sh REL DIR
#
# REL: a `make EFS_FAULTS=1` build tree (efsd, efs-fuse, efs-mgmt).
# DIR: scratch root (e.g. /data1/k22/run); stores, logs, samples.tsv and
#      summary.txt land there, outside Git. Ports PORT+1..PORT+3
#      (PORT default 18500). Kills only its own daemons and mount.
#
# Phases (durations in seconds, env-overridable):
#   1. start nodes 1-2, raft-mkfs, start node 3 (follower of both groups)
#      with EFS_FAULT_COMPACT_STALL=1 and EFS_FAULT_L0_BYTES=$L0 (fault
#      build test hook: the L0 cap that the 1 GiB production cap scales
#      down to, so the bound is reached in minutes); mount one efs-fuse.
#   2. WARM: load with the compactor running (baseline RSS/lag).
#   3. STALL: create the fault file (compactor parks), keep the load;
#      ends PLATEAU s after a leader first reports node 3 busy (deadline
#      STALL).
#   4. RELEASE: remove the fault file, keep the load.
#   5. DRAIN: stop the load; wait until node 3 has applied every commit
#      on both groups (deadline DRAIN).
# Load (LOAD=bench, default): WORKERS loops of `efs-bench --meta` mkdir+
# create rounds of BENCH_FILES files each, direct metadata RPCs (no FUSE),
# so the stalled follower's KV fills in minutes on a small VM. LOAD=fuse:
# WORKERS python processes creating FILE_BYTES-byte files on the mount. Every 5 s per node: VmRSS, raft-status
# bound line (kv_mt_bytes/kv_mem_budget/kv_l0_bytes/kv_l0_cap/
# kv_apply_deferred/admit_busy), per group commit/applied/unapplied/
# max_unapplied/busy_sent and the leader's busy count for node 3, plus
# files created and client errors so far.
set -u
REL=$1; D=$2
PORT=${PORT:-18500}
L0=${L0:-$((4 << 20))}
WARM=${WARM:-60}; STALL=${STALL:-3600}; PLATEAU=${PLATEAU:-300}; RELEASE=${RELEASE:-120}; DRAIN=${DRAIN:-600}
WORKERS=${WORKERS:-2}
FILE_BYTES=${FILE_BYTES:-0}
LOAD=${LOAD:-bench}   # bench: efs-bench --meta (no FUSE); fuse: python creates on the mount
ADDR=127.0.0.1
MNT=$D/mnt
FAULT=$D/fault
S=$D/samples.tsv
export EFS_TRANSPORT=tcp EFS_MD_RAFT_N=3

say() { echo "[k22 $(date -u +%H:%M:%S)] $*" | tee -a "$D/driver.log"; }
pid_of() { cat "$D/logs/n$1.pid" 2>/dev/null; }
cleanup() {
    [ -f "$D/load.stop" ] || touch "$D/load.stop"
    sleep 1
    fusermount3 -uz "$MNT" 2>/dev/null
    for i in 1 2 3; do kill "$(pid_of $i)" 2>/dev/null; done
    sleep 3
    for i in 1 2 3; do kill -9 "$(pid_of $i)" 2>/dev/null; done
    pkill -f "k22_load.py $MNT" 2>/dev/null
    pkill -f "efs-bench $ADDR:$((PORT+1)) --meta" 2>/dev/null
    rm -f "$FAULT"
}
trap cleanup EXIT

for i in 1 2 3; do
    ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }
done
mkdir -p "$D"/{n1,n2,n3,logs,mnt}
rm -f "$FAULT" "$D/load.stop" "$S"
: > "$D/driver.log"

start_node() {  # start_node ID [VAR=val...]
    local i=$1; shift
    local join=""; [ "$i" -gt 1 ] && join="--join $ADDR:$((PORT+1))"
    env EFS_RAFT_OBS=1 "$@" "$REL/efsd" --node-id "$i" --addr $ADDR --port $((PORT+i)) \
        --storage "$D/n$i" --quota 8G --no-direct-io $join \
        >> "$D/logs/efsd-n$i.log" 2>&1 < /dev/null &
    echo $! > "$D/logs/n$i.pid"
    for _ in $(seq 120); do grep -q listening "$D/logs/efsd-n$i.log" && return 0; sleep 0.5; done
    say "FATAL node $i did not start"; exit 2
}
start_node 1
start_node 2
ok=0
for _ in $(seq 40); do
    "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && { ok=1; break; }
    sleep 1
done
[ $ok = 1 ] || { say "FATAL raft-mkfs"; exit 2; }
start_node 3 EFS_FAULT_COMPACT_STALL=1 EFS_FAULT_COMPACT_FILE="$FAULT" EFS_FAULT_L0_BYTES="$L0"
for _ in $(seq 120); do
    st=$("$REL/efs-mgmt" raft-status $ADDR:$((PORT+3)) 2>/dev/null)
    n=$(echo "$st" | grep -c 'hosted=1 role=FOLLOWER')
    [ "$n" = 2 ] && break
    sleep 1
done
"$REL/efs-mgmt" raft-status $ADDR:$((PORT+3)) | tee -a "$D/driver.log"
[ "$n" = 2 ] || { say "FATAL node 3 is not a follower of both groups"; exit 2; }
setsid "$REL/efs-fuse" $ADDR:$((PORT+1)) "$MNT" > "$D/logs/fuse.log" 2>&1 < /dev/null &
for _ in $(seq 100); do findmnt -rn "$MNT" > /dev/null && timeout 3 stat "$MNT" > /dev/null 2>&1 && break; sleep 0.2; done
findmnt -rn "$MNT" > /dev/null || { say "FATAL mount"; tail -5 "$D/logs/fuse.log"; exit 2; }

cat > "$D/k22_load.py" <<'PY'
import os, sys, time
mnt, wid, stop, fb = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
d = os.path.join(mnt, "w%d" % wid)
os.makedirs(d, exist_ok=True)
buf = bytes([0x5a]) * fb
n = err = 0
cnt = os.path.join(os.path.dirname(stop), "load.%d" % wid)
last = 0
while not os.path.exists(stop):
    try:
        with open(os.path.join(d, "f%09d" % n), "wb") as f:
            if buf:
                f.write(buf)
        n += 1
    except OSError:
        err += 1
        time.sleep(0.05)
    if time.time() - last > 1:
        with open(cnt + ".tmp", "w") as f:
            f.write("%d %d\n" % (n, err))
        os.replace(cnt + ".tmp", cnt)
        last = time.time()
with open(cnt, "w") as f:
    f.write("%d %d\n" % (n, err))
PY

T0=$(date +%s)
PHASE=start
sample() {
    local t=$(( $(date +%s) - T0 )) files=0 errs=0 i
    for f in "$D"/load.[0-9]*; do
        [ -f "$f" ] || continue
        read -r a b < "$f"; files=$((files + a)); errs=$((errs + b))
    done
    for i in 1 2 3; do
        local rss st
        rss=$(awk '/VmRSS/{print $2}' /proc/"$(pid_of $i)"/status 2>/dev/null)
        st=$("$REL/efs-mgmt" raft-status $ADDR:$((PORT+i)) 2>/dev/null)
        echo "$st" | awk -v t=$t -v ph=$PHASE -v n=$i -v rss="${rss:-0}" -v files=$files -v errs=$errs '
            function kv(k,  j){for(j=1;j<=NF;j++) if(index($j,k"=")==1) return substr($j,length(k)+2); return ""}
            /^  bound /{mt=kv("kv_mt_bytes");mb=kv("kv_mem_budget");l0=kv("kv_l0_bytes");cap=kv("kv_l0_cap");
                        def=kv("kv_apply_deferred");adm=kv("admit_busy")}
            /^  group /{g=$2; role[g]=kv("role"); c[g]=kv("commit"); a[g]=kv("applied"); u[g]=kv("unapplied");
                        mu[g]=kv("max_unapplied"); bs[g]=kv("busy_sent"); ng++; gs[ng]=g; pb[g]=0}
            /^    peer 2 /{pb[g]=kv("busy")}
            END{for(k=1;k<=ng;k++){x=gs[k];
                printf "%s\t%s\t%d\tnode%d\t%s\tg%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                t,ph,n,n,rss,x,role[x],c[x],a[x],u[x],mu[x],bs[x],pb[x],mt,mb,l0,cap,def,adm,files "\t" errs}}' >> "$S"
    done
}
printf "t_s\tphase\tnode\tname\trss_kb\tgroup\trole\tcommit\tapplied\tunapplied\tmax_unapplied\tbusy_sent\tleader_busy_from_n3\tkv_mt_bytes\tkv_mem_budget\tkv_l0_bytes\tkv_l0_cap\tkv_apply_deferred\tadmit_busy\tfiles\terrors\n" > "$S"
run_phase() {  # run_phase NAME SECONDS [PLATEAU]
    PHASE=$1; local end=$(( $(date +%s) + $2 )) pl=${3:-0} busy_at=0
    say "phase $1 for up to $2 s"
    while [ "$(date +%s)" -lt "$end" ]; do
        sample
        if [ "$pl" -gt 0 ] && [ "$busy_at" = 0 ] &&
           tail -n 6 "$S" | awk -F'\t' '$7=="LEADER" && $13>0{f=1} END{exit !f}'; then
            busy_at=$(date +%s); end=$(( busy_at + pl ))
            say "leader reports node 3 busy; plateau window $pl s"
        fi
        sleep 5
    done
}

BENCH_FILES=${BENCH_FILES:-20000}
bench_loop() {  # bench_loop W: rounds until load.stop; counter file load.W
    local w=$1 n=0 e=0 r=0
    while [ ! -f "$D/load.stop" ]; do
        r=$((r + 1))
        if "$REL/efs-bench" $ADDR:$((PORT+1)) --meta --files "$BENCH_FILES" --dirs 16 \
               --workers 16 --phases mkdir,create --keep --id $((w * 100000 + r)) \
               >> "$D/logs/load$w.log" 2>&1; then
            n=$((n + BENCH_FILES))
        else
            e=$((e + 1))
        fi
        echo "$n $e" > "$D/load.$w"
    done
}
for w in $(seq 1 "$WORKERS"); do
    if [ "$LOAD" = fuse ]; then
        python3 "$D/k22_load.py" "$MNT" "$w" "$D/load.stop" "$FILE_BYTES" > "$D/logs/load$w.log" 2>&1 &
    else
        bench_loop "$w" &
    fi
done
run_phase warm "$WARM"
touch "$FAULT"
run_phase stall "$STALL" "$PLATEAU"
rm -f "$FAULT"
run_phase release "$RELEASE"
touch "$D/load.stop"; pkill -f "efs-bench $ADDR:$((PORT+1)) --meta"; sleep 2
PHASE=drain
say "phase drain (deadline $DRAIN s)"
drain_t0=$(date +%s); caught=0
while [ $(( $(date +%s) - drain_t0 )) -lt "$DRAIN" ]; do
    sample
    lagging=$(tail -n 6 "$S" | awk -F'\t' '$3==3 && ($8!=$9 || $10!=0)' | wc -l)
    lead=$("$REL/efs-mgmt" raft-status $ADDR:$((PORT+1)) 2>/dev/null | awk '/^  group/{for(j=1;j<=NF;j++) if(index($j,"commit=")==1) print substr($j,8)}' | tr '\n' ' ')
    n3=$("$REL/efs-mgmt" raft-status $ADDR:$((PORT+3)) 2>/dev/null | awk '/^  group/{for(j=1;j<=NF;j++) if(index($j,"applied=")==1) print substr($j,9)}' | tr '\n' ' ')
    if [ "$lagging" = 0 ]; then caught=1; break; fi
    sleep 5
done
say "drain caught_up=$caught after $(( $(date +%s) - drain_t0 )) s (leader commits: $lead; node3 applied: $n3)"
sample

# Summary from samples.tsv: per phase and node, peaks.
awk -F'\t' 'NR>1{k=$2" node"$3; if($5>rss[k])rss[k]=$5; if($10>un[k])un[k]=$10; if($14>mt[k])mt[k]=$14;
    if($16>l0[k])l0[k]=$16; if($12>bs[k])bs[k]=$12; if($18>df[k])df[k]=$18; if($19>ad[k])ad[k]=$19;
    mu[k]=$11; mb[k]=$15; cap[k]=$17; f[$2]=$20; e[$2]=$21; keys[k]=1}
    END{for(k in keys) printf "%s peak_rss_kb=%d peak_unapplied=%d max_unapplied=%d peak_mt=%d mem_budget=%d peak_l0=%d l0_cap=%d busy_sent=%d apply_deferred=%d admit_busy=%d\n",
        k,rss[k],un[k],mu[k],mt[k],mb[k],l0[k],cap[k],bs[k],df[k],ad[k];
        for(p in f) printf "%s files=%s errors=%s\n", p, f[p], e[p]}' "$S" | sort > "$D/summary.txt"
grep -h 'kv-bound:\|kv-fault:\|AE busy' "$D"/logs/efsd-n3.log | head -20 >> "$D/summary.txt"
echo "caught_up=$caught" >> "$D/summary.txt"
cat "$D/summary.txt"
