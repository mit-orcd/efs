# Shared helpers for tests/measure/*.sh — source, do not run.
#
# Every measurement script: (1) runs tests/preflight.sh and stops on FAIL,
# (2) writes to results/measure/<date>-<name>/ (preflight.txt + raw + a
# SUMMARY.txt with the numbers to hand back), (3) issues only short ssh
# commands itself and puts anything long behind efs-bg.sh or a detached
# harness. Run the scripts themselves via efs-bg.sh (screen on node9901,
# fstor007, or an fcstor). Anything estimated over 10 s stays out of the
# login-node shell.
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
REPO="${REPO:-$HOME/git/efs}"
ALL_CLIENTS="fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015"
SERVERS="fcstor003 fcstor004 fcstor005 fcstor006"

# ssh_ <timeout-s> <host-without-.ib> '<cmd>'
ssh_() { local t=$1 h=$2; shift 2; EFS_SSH_TIMEOUT=$t "$SSH" "$h.ib" "$@" 2>&1 | grep -v '^Identity added'; }

say() { echo "[$(basename "$0" .sh)] $*"; }
n0()  { echo "${1:-0}"; }   # empty → 0 for arithmetic

# mkout <name> → sets OUT=results/measure/<UTC stamp>-<name>, creates it
mkout() {
    STAMP=$(date -u +%Y%m%d-%H%M%S)
    OUT="${OUT:-$REPO/results/measure/$STAMP-$1}"
    mkdir -p "$OUT"
    say "results -> $OUT"
}

# preflight_or_die [--expect-build id] : runs tests/preflight.sh, saves output
preflight_or_die() {
    bash "$REPO/tests/preflight.sh" "$@" > "$OUT/preflight.txt" 2>&1
    local rc=$?
    grep -E 'PREFLIGHT_(OK|FAIL)|^FAIL' "$OUT/preflight.txt"
    [ $rc = 0 ] || { say "preflight FAILED — fix the cause (deploy rule), do not measure"; exit 1; }
    BUILD=$(grep -o 'PREFLIGHT_OK build=[^ ]*' "$OUT/preflight.txt" | cut -d= -f2)
}

# leader_host <group> → fcstor00N that is LEADER for the group (or empty)
leader_host() {
    local g=$1 h role
    for h in $SERVERS; do
        role=$(ssh_ 10 "$h" "cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.$(( ${h#fcstor00} + 54 )):19810 2>/dev/null | grep 'group $g ' | grep -o 'role=[A-Z]*'")
        [ "$role" = "role=LEADER" ] && { echo "$h"; return 0; }
    done
    return 1
}

# remount_clients "<hosts>" '<extra env for efs-fuse>' : same binary, no build
remount_clients() {
    EFS_NO_BUILD=1 EFS_FUSE_ENV="$2" bash "$REPO/tests/deploy_fuse_clients.sh" $1
}

# rpc_prof_last <host> → the last RPC-PROF line of that client's fuse.log.
# The client prints the cumulative line only when an RPC completes and at
# most every 2 s, so a client that went quiet never prints its final
# totals: wait out the 2 s, issue two trivial RPCs (getattr of the mount
# root — the first one triggers the dump, the second is counted in it),
# then read the last line.
rpc_prof_last() {
    ssh_ 15 "$1" "sleep 2.2; stat /tmp/efs-mount/ >/dev/null 2>&1; sleep 0.3; stat /tmp/efs-mount/ >/dev/null 2>&1; sleep 0.3; grep RPC-PROF /tmp/efs/fuse.log | tail -1"
}

# rpc_field "<line>" <name> → value of name=... ; for name=n/us fields use
# rpc_field_n / rpc_field_us
rpc_field()    { echo "$1" | grep -o " $2=[0-9]*" | head -1 | cut -d= -f2; }
rpc_field_n()  { echo "$1" | grep -o " $2=[0-9]*/" | head -1 | tr -dc '0-9'; }
rpc_field_us() { echo "$1" | grep -o " $2=[0-9]*/[0-9]*us" | head -1 | cut -d/ -f2 | tr -dc '0-9'; }

# Server-side REPORT timing. efsd logs
#   report-split nrec=N pack_ms=A push_ms=B finish_ms=C rc=R
# for every REPORT_CHUNKS with >=256 recs or >=100 ms total (pack = build
# PUBLISH batches, push = propose to both groups, finish = wait for
# apply). The client's RPC-PROF does NOT count REPORT on this build
# (rpc_send_recv_dual is unprofiled), so this is the REPORT number.
# srv_log_mark : remember each server's efsd.log length;
# srv_report_split <outfile> : new report-split lines since the mark + a
# one-line summary (count, sum and max of each phase).
declare -A SRV_MARK
srv_log_mark() { local h; for h in $SERVERS; do SRV_MARK[$h]=$(ssh_ 10 "$h" 'wc -l < /tmp/efs/efsd.log'); done; }
srv_report_split() {
    local h out=$1; : > "$out"
    for h in $SERVERS; do
        ssh_ 15 "$h" "tail -n +$(( ${SRV_MARK[$h]:-0} + 1 )) /tmp/efs/efsd.log | grep report-split | sed 's/^/$h /'" >> "$out"
    done
    awk '{n++; for(i=3;i<=6;i++){split($i,a,"="); s[a[1]]+=a[2]; if(a[2]>m[a[1]])m[a[1]]=a[2]}}
         END{if(n==0){print "report-split: none logged (all REPORTs <256 recs and <100 ms)"; exit}
             printf "report-split: %d lines; pack_ms sum=%d max=%d; push_ms sum=%d max=%d; finish_ms sum=%d max=%d\n",
                    n, s["pack_ms"],m["pack_ms"], s["push_ms"],m["push_ms"], s["finish_ms"],m["finish_ms"]}' "$out"
}

# raft_hist <host> → command histogram of the last 4 MiB of that node's raft.log
raft_hist() { ssh_ 15 "$1" "timeout 10 python3 \$HOME/git/efs/tests/tools/raft_log_tail.py /data1/01/efs/mdraft/log/raft.log 2>&1 | head -16"; }

# perf_daemon <host> <procname> <secs> <outfile-basename>
# perf record on the RUNNING daemon (never start one under a profiler),
# report saved on the node in /tmp and copied into $OUT.
perf_daemon() {
    local h=$1 p=$2 s=$3 b=$4
    ssh_ $((s + 60)) "$h" "pid=\$(pgrep -x $p | head -1); [ -n \"\$pid\" ] || { echo no-$p; exit 1; }
        perf record -q -e cpu-clock -g --call-graph fp -p \$pid -o /tmp/perf-$b.data sleep $s >/dev/null 2>&1
        timeout 40 perf report --stdio --no-children -n -i /tmp/perf-$b.data 2>/dev/null | grep -v '^#' | grep -v '^$' | head -60" > "$OUT/perf-$b-$h.txt"
    say "perf $p on $h -> $OUT/perf-$b-$h.txt ($(wc -l < "$OUT/perf-$b-$h.txt") lines)"
}

# ensure_src <host> : 8 GiB non-zero source file /tmp/src8g (dd if=/dev/zero
# skips PUTs — all-zero chunks are not written)
ensure_src() {
    ssh_ 90 "$1" 'python3 - <<"PY"
import os
p="/tmp/src8g"; want=8<<30
if os.path.exists(p) and os.path.getsize(p)==want:
    print("SRC8G_OK"); raise SystemExit(0)
b=b"\x5a"*(1<<20)
with open(p,"wb") as f:
    for _ in range(8192): f.write(b)
print("SRC8G_MADE")
PY'
}
