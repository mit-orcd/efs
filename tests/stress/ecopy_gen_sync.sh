#!/bin/bash
# ecopy_gen_sync.sh — one-client copy that must NOT split committed gens.
#
# Repeatable gate for the 2PC joiners-stuck-at-gen-2/5 bug. From the login
# node, after a fresh cluster:
#
#   bash tests/clean_cluster.sh && bash tests/run_tests.sh setup
#   bash tests/stress/ecopy_gen_sync.sh
#
# Workload (one client, FUSE export — not local disk):
#   ~/git/direct_copy/ecopy ~/orcd/scratch/ecrawl-synt-small/ /tmp/efs-mount/
#
# ecopy SRC DST copies the CONTENTS of SRC into DST (not SRC as a subdir).
# The tree is ~6M files; finishing it is not the gate. Default window is
# 180s — enough to drive gens well past the split (joiners pinned at 2/5
# while the primary ran to thousands). A timeout still PASSes if all 4
# committed gens match. Raise ECOPY_TIMEOUT to copy longer.
#
# TRACE=1 (default): perf record on the primary's existing efsd, strace -c
# on one joiner, TRACE_SEC seconds from ecopy start. Does NOT wrap efsd
# in strace (that leaves /tmp/efs-mount as local md0).
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
CLIENT="${CLIENT:-fcstor007.ib}"
MNT="${MNT:-/tmp/efs-mount}"
SRC="${SRC:-$HOME/orcd/scratch/ecrawl-synt-small}"
ECOPY="${ECOPY:-$HOME/git/direct_copy/ecopy}"
PRIMARY="${PRIMARY:-fcstor003.ib}"
JOINER="${JOINER:-fcstor004.ib}"
MGMT="${MGMT:-172.16.223.57:19810}"
ECOPY_TIMEOUT="${ECOPY_TIMEOUT:-180}"
TRACE="${TRACE:-1}"
TRACE_SEC="${TRACE_SEC:-25}"

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="${OUTDIR:-$HOME/git/efs/results/ecopy-gen-sync/$STAMP}"
mkdir -p "$OUTDIR"
say() { echo "[gen-sync] $*"; }

ssh_to() { local t=$1; shift; EFS_SSH_TIMEOUT=$t "$SSH" "$@"; }

fstype=$(ssh_to 10 "$CLIENT" "findmnt -n -o FSTYPE $MNT 2>/dev/null" | tr -d '\r')
if [ "$fstype" != "fuse.efs-fuse" ]; then
    say "FAIL: $CLIENT:$MNT is '$fstype' (need fuse.efs-fuse). Run: bash tests/run_tests.sh setup"
    exit 2
fi
say "FUSE_OK $CLIENT:$MNT"

if [ ! -x "$ECOPY" ]; then
    say "FAIL: ecopy not executable: $ECOPY"
    exit 2
fi
if [ ! -d "$SRC" ]; then
    say "FAIL: source missing: $SRC"
    exit 2
fi

gens() {
    ssh_to 15 "$PRIMARY" "cd /tmp/efs && ./efs-mgmt status $MGMT" \
        | grep -oE 'gen=[0-9]+' | sed 's/gen=//' | tr '\n' ' '
}

say "gens before: $(gens)"

if [ "$TRACE" = 1 ]; then
    say "trace $TRACE_SEC s: perf record on $PRIMARY, strace -c on $JOINER (existing pids; not wrapping efsd)"
    ssh_to 15 "$PRIMARY" "
        pid=\$(pgrep -x efsd)
        echo pid=\$pid
        rm -f /tmp/perf-efsd-gensync.data
        timeout $((TRACE_SEC + 5)) perf record -e cpu-clock -g --call-graph fp \
            -p \$pid -o /tmp/perf-efsd-gensync.data sleep $TRACE_SEC \
            >/tmp/perf-efsd-gensync.out 2>&1 &
        echo TRACE_PID=\$pid
    " | tee "$OUTDIR/trace-start-primary.txt"
    ssh_to 15 "$JOINER" "
        pid=\$(pgrep -x efsd)
        echo pid=\$pid
        rm -f /tmp/strace-efsd-gensync.txt
        timeout $((TRACE_SEC + 5)) strace -cf -p \$pid \
            -o /tmp/strace-efsd-gensync.txt >/tmp/strace-efsd-gensync.out 2>&1 &
        echo TRACE_PID=\$pid
    " | tee "$OUTDIR/trace-start-joiner.txt"
fi

say "ecopy $SRC -> $CLIENT:$MNT (timeout ${ECOPY_TIMEOUT}s)"
# Contents of SRC land in MNT (ecopy does not create a named subdir).
t0=$(date +%s.%N)
set +e
ssh_to "$ECOPY_TIMEOUT" "$CLIENT" \
    "mkdir -p $MNT && $ECOPY '$SRC' '$MNT'" \
    >"$OUTDIR/ecopy.log" 2>&1
erc=$?
set -e
t1=$(date +%s.%N)
wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')
say "ecopy rc=$erc wall=${wall}s  log=$OUTDIR/ecopy.log"

if [ "$TRACE" = 1 ]; then
    sleep 2
    ssh_to 40 "$PRIMARY" "
        if [ -f /tmp/perf-efsd-gensync.data ]; then
            timeout 20 perf report --stdio --no-children -n \
                -i /tmp/perf-efsd-gensync.data 2>/dev/null | head -40
            echo PERF_OK
        else
            echo PERF_MISSING
            cat /tmp/perf-efsd-gensync.out 2>/dev/null | tail -20
        fi
    " >"$OUTDIR/trace-primary.txt" 2>&1
    ssh_to 20 "$JOINER" "
        if [ -s /tmp/strace-efsd-gensync.txt ]; then
            cat /tmp/strace-efsd-gensync.txt
            echo STRACE_OK
        else
            echo STRACE_MISSING
            cat /tmp/strace-efsd-gensync.out 2>/dev/null | tail -20
        fi
    " >"$OUTDIR/trace-joiner.txt" 2>&1
fi

# Catchup can lag one flush; poll up to 30s for a single committed gen.
ok=0
g=""
for i in 1 2 3 4 5 6; do
    g=$(gens)
    set -- $g
    say "gens poll $i: $g"
    if [ $# -ge 4 ]; then
        u=$(printf '%s\n' "$@" | sort -u | wc -l)
        if [ "$u" -eq 1 ]; then
            ok=1
            break
        fi
    fi
    sleep 5
done
echo "$g" >"$OUTDIR/gens.txt"

set +e
files=$(ssh_to 15 "$CLIENT" "timeout 10 find $MNT -xdev -type f -printf . 2>/dev/null | wc -c" | tr -d '[:space:]')
set -e
case "${files:-}" in
    ''|*[!0-9]*) files=0 ;;
esac
say "files at dest: $files"
echo "files=$files erc=$erc wall=$wall gens=$g" >"$OUTDIR/summary.txt"

# 124 = copy window ended (tree is ~6M files). Still require gen-sync.
if [ "$erc" -ne 0 ] && [ "$erc" -ne 124 ]; then
    say "FAIL: ecopy rc=$erc"
    exit 1
fi
if [ "$files" -lt 1 ]; then
    say "FAIL: dest has no files (copy did not land on FUSE)"
    exit 1
fi
if [ "$ok" -ne 1 ]; then
    say "FAIL: committed gens diverged after copy: $g"
    say "      (all 4 servers must share one gen — this is the 2PC split)"
    exit 1
fi
say "PASS gens=$g files=$files wall=${wall}s -> $OUTDIR"
exit 0
