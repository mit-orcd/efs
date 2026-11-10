#!/bin/bash
# Task FD: one 35 s blocked-sink run of the test's first phase (the profile
# CLI with the test's arguments, fixture kept), with strace on the three
# daemons for the middle 20 s: every fsync/fdatasync/sync_file_range with the
# path of its file descriptor, so the fsyncs per write can be counted.
# Usage, from a built tree root with the build environment set:
#   bash scripts/evidence/pump-logging/pump_logging_fsync_breakdown.sh TAG OUTDIR
# Writes OUTDIR/TAG/{profile.log,strace-n1..3.txt,fixture/} (fixture: result.json and daemon logs).
set -u
tag=$1; out=$2/$tag
work=${TMPDIR:-/tmp}/fd-bd-$tag
mkdir -p "$out"
[ ! -e "$work" ] || { echo "$work exists" >&2; exit 2; }
python3 scripts/evidence/pump-logging/pump_logging_profile.py --work "$work/work" --output "$work/out" \
    --sink blocked --obs 1 --seconds 35 --threads 2 --rate 2 --slow-fsync-us 40000 --versioned \
    --require-progress > "$out/profile.log" 2>&1 &
pp=$!
until [ -e "$work/out/raft-before-n1.txt" ]; do kill -0 $pp 2>/dev/null || exit 3; sleep 0.2; done
sleep 4                               # the window has started; skip the first writes
spids=""
for i in 1 2 3; do
    pid=$(pgrep -u "$(id -u)" -f "tests/faults/efsd .*$work/work/cluster/n$i( |$)" | head -1)
    [ -n "$pid" ] || { echo "no efsd n$i" >&2; continue; }
    echo "n$i pid $pid" >> "$out/pids.txt"
    strace -f -y -tt -e trace=fsync,fdatasync,sync_file_range -p "$pid" -o "$out/strace-n$i.txt" 2>/dev/null &
    spids="$spids $!"
done
sleep 20
for s in $spids; do kill -INT "$s" 2>/dev/null; done
wait $spids 2>/dev/null
wait $pp; echo "profile rc=$?" >> "$out/profile.log"
mkdir -p "$out/fixture"
cp "$work"/out/n?.log "$work"/out/n?-sync.log "$out/fixture/" 2>/dev/null
python3 - "$work/out/result.json" "$out/fixture/result.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
r.pop('samples', None)
json.dump(r, open(sys.argv[2], 'w'))
PY
