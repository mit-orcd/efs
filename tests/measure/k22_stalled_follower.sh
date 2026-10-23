#!/bin/bash
# K22 / W23: a follower stopped with SIGSTOP during sustained metadata load,
# then resumed. Private 3-daemon cluster on one host (TCP, buffered I/O).
#
#   bash tests/measure/k22_stalled_follower.sh REL DIR
#
# REL: a build tree (efsd, efs-mgmt, efs-bench). DIR: absolute scratch root;
# stores, logs, samples.tsv and driver.log land there, outside Git. Ports
# PORT+1..PORT+3 (PORT default 19013). Signals and stops only its own PIDs.
#
# Phases (seconds, env-overridable):
#   WARM   load with all three nodes running.
#   STOP   SIGSTOP node 3 and keep the load until a leader of each group has
#          a log floor above node 3's match index (node 3 then needs an
#          InstallSnapshot), deadline STOP.
#   AFTER  keep the load HOLD s more, then SIGCONT node 3 and keep the load
#          until node 3 has applied the leader commit of both groups.
#   DRAIN  stop the load, wait until node 3 matches both leaders (DRAIN).
# Load: WORKERS loops of `efs-bench --meta` mkdir+create rounds of
# BENCH_FILES files with 16 workers each (direct metadata RPCs). Every 5 s
# per node: VmRSS, raft-status bound line and per group role, commit,
# applied, unapplied, snap, last, admit_busy, busy_sent, and the leaders'
# peer-2 (node 3) match and busy.
set -u
REL=$1; D=$2
PORT=${PORT:-19013}
WARM=${WARM:-60}; STOP=${STOP:-4500}; HOLD=${HOLD:-30}; DRAIN=${DRAIN:-900}
WORKERS=${WORKERS:-8}; BENCH_FILES=${BENCH_FILES:-5000}
ADDR=127.0.0.1; S=$D/samples.tsv
export EFS_TRANSPORT=tcp

say() { echo "[k22s $(date -u +%H:%M:%S)] $*" | tee -a "$D/driver.log"; }
pid_of() { cat "$D/logs/n$1.pid" 2>/dev/null; }
cleanup() {
    touch "$D/load.stop"
    kill -CONT "$(pid_of 3)" 2>/dev/null
    for w in $(cat "$D/logs/load.pids" 2>/dev/null); do kill "$w" 2>/dev/null; done
    pkill -f "efs-bench $ADDR:$((PORT+1)) --meta" 2>/dev/null
    for i in 1 2 3; do kill "$(pid_of $i)" 2>/dev/null; done
    sleep 3
    for i in 1 2 3; do kill -9 "$(pid_of $i)" 2>/dev/null; done
}
trap cleanup EXIT
for i in 1 2 3; do
    ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }
done
mkdir -p "$D"/{n1,n2,n3,logs}; rm -f "$D/load.stop" "$S" "$D/logs/load.pids"; : > "$D/driver.log"
start_node() {
    local i=$1 join=""; [ "$i" -gt 1 ] && join="--join $ADDR:$((PORT+1))"
    "$REL/efsd" --node-id "$i" --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" --quota 16G \
        --no-direct-io $join >> "$D/logs/efsd-n$i.log" 2>&1 < /dev/null &
    echo $! > "$D/logs/n$i.pid"
    for _ in $(seq 120); do grep -q listening "$D/logs/efsd-n$i.log" && return 0; sleep 0.5; done
    say "FATAL node $i did not start"; exit 2
}
start_node 1; start_node 2
for _ in $(seq 40); do
    "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1
done
start_node 3
for _ in $(seq 120); do
    [ "$("$REL/efs-mgmt" raft-status $ADDR:$((PORT+3)) 2>/dev/null | grep -c 'hosted=1 role=FOLLOWER')" = 2 ] && break
    sleep 1
done
"$REL/efs-mgmt" raft-status $ADDR:$((PORT+3)) | tee -a "$D/driver.log"

T0=$(date +%s); PHASE=warm
printf "t_s\tphase\tnode\trss_kb\tgroup\trole\tcommit\tapplied\tunapplied\tmax_unapplied\tsnap\tlast\tadmit_busy\tbusy_sent\tn3_match\tn3_busy\tkv_mt_bytes\tkv_mem_budget\tkv_l0_bytes\tfiles\terrors\n" > "$S"
sample() {
    local t=$(( $(date +%s) - T0 )) files=0 errs=0 i f a b
    for f in "$D"/load.[0-9]*; do [ -f "$f" ] && read -r a b < "$f" && files=$((files + a)) && errs=$((errs + b)); done
    for i in 1 2 3; do
        [ "$i" = 3 ] && { [ "$PHASE" = stopped ] || [ "$PHASE" = hold ]; } && continue  # stopped node
        local rss st
        rss=$(awk '/VmRSS/{print $2}' /proc/"$(pid_of $i)"/status 2>/dev/null)
        st=$(timeout 10 "$REL/efs-mgmt" raft-status $ADDR:$((PORT+i)) 2>/dev/null)
        echo "$st" | awk -v t=$t -v ph=$PHASE -v n=$i -v rss="${rss:-0}" -v files=$files -v errs=$errs '
            function kv(k,  j){for(j=1;j<=NF;j++) if(index($j,k"=")==1) return substr($j,length(k)+2); return ""}
            /^  bound /{mt=kv("kv_mt_bytes");mb=kv("kv_mem_budget");l0=kv("kv_l0_bytes")}
            /^  group /{g=$2; ng++; gs[ng]=g; role[g]=kv("role"); c[g]=kv("commit"); a[g]=kv("applied");
                        u[g]=kv("unapplied"); mu[g]=kv("max_unapplied"); sn[g]=kv("snap"); la[g]=kv("last");
                        ab[g]=kv("admit_busy"); bs[g]=kv("busy_sent"); pm[g]="-"; pb[g]="-"}
            /^    peer 2 /{pm[g]=kv("match"); pb[g]=kv("busy")}
            END{for(k=1;k<=ng;k++){x=gs[k];
                printf "%s\t%s\t%d\t%s\tg%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                t,ph,n,rss,x,role[x],c[x],a[x],u[x],mu[x],sn[x],la[x],ab[x],bs[x],pm[x],pb[x],mt,mb,l0,files,errs}}' >> "$S"
    done
}
bench_loop() {
    local w=$1 n=0 e=0 r=0
    while [ ! -f "$D/load.stop" ]; do
        r=$((r + 1))
        if "$REL/efs-bench" $ADDR:$((PORT+1)) --meta --files "$BENCH_FILES" --dirs 16 --workers 16 \
               --phases mkdir,create --keep --id $((w * 100000 + r)) >> "$D/logs/load$w.log" 2>&1; then
            n=$((n + BENCH_FILES)); else e=$((e + 1)); fi
        echo "$n $e" > "$D/load.$w"
    done
}
for w in $(seq 1 "$WORKERS"); do bench_loop "$w" & echo $! >> "$D/logs/load.pids"; done

# The leader keeps EFS_RAFT_SNAP_BYTES of log after a snapshot, so node 3
# needs InstallSnapshot once a group's leader has taken two snapshots past
# node 3's match index (the second one drops the window covering it).
leader_floor_past_n3() {
    awk -F'\t' '$2=="stopped" && $6=="LEADER" && $15!="-" && $11+0 > $15+0 {
            if (!((($5) SUBSEP $11) in seen)) { seen[$5, $11] = 1; n[$5]++ } }
        END{k=0; for (g in n) if (n[g] >= 2) k++; exit k==2?0:1}' "$S"
}
say "phase warm $WARM s"; end=$(( $(date +%s) + WARM ))
while [ "$(date +%s)" -lt "$end" ]; do sample; sleep 5; done
PHASE=stopped; kill -STOP "$(pid_of 3)"; say "SIGSTOP node 3 pid $(pid_of 3)"
end=$(( $(date +%s) + STOP )); hit=0
while [ "$(date +%s)" -lt "$end" ]; do
    sample
    if leader_floor_past_n3; then hit=1; say "every leader snapshotted past node 3's match"; break; fi
    sleep 5
done
[ $hit = 1 ] || say "deadline: no leader snapshotted past node 3's match"
PHASE=hold; end=$(( $(date +%s) + HOLD ))
while [ "$(date +%s)" -lt "$end" ]; do sample; sleep 5; done
PHASE=resumed; kill -CONT "$(pid_of 3)"; say "SIGCONT node 3"; t_cont=$(date +%s); caught=0
while [ $(( $(date +%s) - t_cont )) -lt "$DRAIN" ]; do
    sample
    if tail -n 6 "$S" | awk -F'\t' '$3==3 && $8+0>0{a[$5]=$8} $6=="LEADER"{c[$5]=$7}
        END{n=0; for (g in c) { if (!(g in a) || a[g]+2000 < c[g]) exit 1; n++ } exit n==2?0:1}'; then
        caught=1; say "node 3 within 2000 entries of both leaders' commit after $(( $(date +%s) - t_cont )) s under load"; break
    fi
    sleep 5
done
[ $caught = 1 ] || say "node 3 did not catch up under load within $DRAIN s"
end=$(( $(date +%s) + 60 )); while [ "$(date +%s)" -lt "$end" ]; do sample; sleep 5; done
touch "$D/load.stop"; PHASE=drain; say "phase drain"; t_d=$(date +%s)
while [ $(( $(date +%s) - t_d )) -lt "$DRAIN" ]; do
    sample
    if tail -n 6 "$S" | awk -F'\t' '$3==3{a[$5]=$8} $6=="LEADER"{c[$5]=$7}
        END{n=0; for (g in c) { if (a[g] != c[g]) exit 1; n++ } exit n==2?0:1}'; then
        say "drain: node 3 applied every commit on both groups after $(( $(date +%s) - t_d )) s"; break
    fi
    sleep 5
done
grep -iE "snapshot|install|snap" "$D/logs/efsd-n3.log" | tail -20 >> "$D/driver.log"
say "done"
