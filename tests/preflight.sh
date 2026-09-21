#!/bin/bash
# preflight.sh [--expect-build <id>] [--clients "fcstor007 ... "] [--quiet-secs N]
#
# The fcstor deploy rule's pre-flight as one read-only command. Run it
# BEFORE anything that touches the 19810 cluster and paste its output into
# the result directory. Every check is a short ssh; ~30 s total (the
# commit-flat check waits --quiet-secs, default 15).
#
#   1. servers: one efsd on each of fcstor003-006, same build string
#   2. build string == --expect-build when given (runbooks pin the version)
#   3. raft: both groups have a leader, commit == applied on every voter
#   4. commit is (nearly) flat over --quiet-secs on both groups
#      (> 5 entries/s idle = something is running; do not measure yet)
#   5. clients: findmnt fuse.efs-fuse + stat OK + exactly one efs-fuse
#   6. leftover load: no io500 / ior / fio / mdtest / python3 storm on clients
#
# Exit 0 + "PREFLIGHT_OK build=<id>" or exit 1 with the failing lines
# marked FAIL. A FAIL is a stop: fix the cause (deploy rule), never skip.
set -u
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
EXPECT=""; CLIENTS="fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015"
QUIET=15
while [ $# -gt 0 ]; do
    case "$1" in
    --expect-build) EXPECT=$2; shift 2;;
    --clients) CLIENTS=$2; shift 2;;
    --quiet-secs) QUIET=$2; shift 2;;
    *) echo "unknown arg $1" >&2; exit 2;;
    esac
done
ssh_() { local t=$1 h=$2; shift 2; EFS_SSH_TIMEOUT=$t "$S" "$h.ib" "$@" 2>&1 | grep -v '^Identity added'; }
fail=0
bad() { echo "FAIL: $*"; fail=1; }

echo "== preflight $(date -u +%FT%TZ) from $(hostname -s)"

# 1+2 servers
builds=""
for n in 1 2 3 4; do
    h="fcstor00$((n + 2))"
    out=$(ssh_ 10 "$h" 'echo "$(pgrep -x efsd | wc -l) $(grep -o "build=[^ ]*" /tmp/efs/efsd.log | head -1)"')
    cnt=${out%% *}; b=${out#* }
    echo "server $h efsd=$cnt $b"
    [ "$cnt" = 1 ] || bad "$h has $cnt efsd (expect 1)"
    builds="$builds ${b#build=}"
done
nb=$(echo $builds | tr ' ' '\n' | sort -u | wc -l)
[ "$nb" = 1 ] || bad "server build strings differ:$builds"
build=$(echo $builds | awk '{print $1}')
[ -z "$EXPECT" ] || [ "$build" = "$EXPECT" ] || bad "running build $build != expected $EXPECT"

# 3 raft health (from node 2, which hosts both groups)
st=$(ssh_ 15 fcstor004 'cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.58:19810 2>/dev/null | grep hosted=1')
[ -n "$st" ] || bad "raft-status returned nothing (efs-mgmt missing on fcstor004? build it: make efs-mgmt)"
echo "$st" | sed 's/^/raft /'
while read -r line; do
    [ -n "$line" ] || continue
    g=$(echo "$line" | grep -o 'group [0-9]*' | awk '{print $2}')
    ld=$(echo "$line" | grep -o 'leader=[-0-9]*' | cut -d= -f2)
    c=$(echo "$line" | grep -o 'commit=[0-9]*' | cut -d= -f2)
    a=$(echo "$line" | grep -o 'applied=[0-9]*' | cut -d= -f2)
    [ "$ld" != "-1" ] || bad "group $g has no leader"
    [ "$c" = "$a" ] || bad "group $g commit=$c != applied=$a (catching up — wait)"
done <<< "$st"
c0a=$(echo "$st" | grep 'group 0' | grep -o 'commit=[0-9]*' | cut -d= -f2)
c2a=$(echo "$st" | grep 'group 2' | grep -o 'commit=[0-9]*' | cut -d= -f2)

# 5 clients + 6 leftover load (parallel)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
for h in $CLIENTS; do
    ( ssh_ 10 "$h" 'echo "$(findmnt -no FSTYPE /tmp/efs-mount 2>/dev/null || echo none) $(stat /tmp/efs-mount/ >/dev/null 2>&1 && echo MOUNT_OK || echo MOUNT_BAD) fuse=$(pgrep -x efs-fuse | wc -l) io500=$(pgrep -x io500 | wc -l) ior=$(pgrep -x ior | wc -l) mdtest=$(pgrep -x mdtest | wc -l) fio=$(pgrep -x fio | wc -l) py=$(pgrep -x python3 | wc -l) $(grep -o "build=[^ ]*" /tmp/efs/fuse.log 2>/dev/null | head -1)"' > "$tmp/$h" ) &
done
wait
for h in $CLIENTS; do
    out=$(cat "$tmp/$h")
    echo "client $h $out"
    echo "$out" | grep -q '^fuse.efs-fuse MOUNT_OK fuse=1 ' || bad "$h mount not usable ($out) — deploy rule 'Restart one efs-fuse'"
    echo "$out" | grep -qE 'io500=0 ior=0 mdtest=0 fio=0 ' || bad "$h has leftover load ($out) — pkill -9 -x <name>, never -f"
    py=$(echo "$out" | grep -o 'py=[0-9]*' | cut -d= -f2)
    [ "${py:-0}" -le 2 ] || bad "$h has $py python3 processes (a posix/storm leftover?)"
done

# 4 commit flat
sleep "$QUIET"
st2=$(ssh_ 15 fcstor004 'cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.58:19810 2>/dev/null | grep hosted=1')
c0b=$(echo "$st2" | grep 'group 0' | grep -o 'commit=[0-9]*' | cut -d= -f2)
c2b=$(echo "$st2" | grep 'group 2' | grep -o 'commit=[0-9]*' | cut -d= -f2)
r0=$(( (${c0b:-0} - ${c0a:-0}) / QUIET )); r2=$(( (${c2b:-0} - ${c2a:-0}) / QUIET ))
echo "commit rate over ${QUIET}s: group0 ${r0}/s group2 ${r2}/s (idle: 0-2/s = lease/reaper tail)"
[ "$r0" -le 5 ] && [ "$r2" -le 5 ] || bad "cluster not idle (>5 entries/s) — find the load before measuring (tests/tools/raft_log_tail.py on the leader's raft.log)"

if [ $fail = 0 ]; then echo "PREFLIGHT_OK build=$build"; exit 0; fi
echo "PREFLIGHT_FAIL build=$build"; exit 1
