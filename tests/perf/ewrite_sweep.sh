#!/bin/bash
# Timed ewrite.sh concurrency sweep. Runs ON one client against a live mount.
#
# Usage: ewrite_sweep.sh <mnt> <out-tsv>
#
# For each job count in EWRITE_JOBS (default: 2 4 8 16) launches
#   ewrite.sh <dest> 1 <jobs>
# (ewrite.sh defaults: 102400 x 1 MiB per file), then SIGKILL after
# EWRITE_SECS (default 30) and records bytes written / wall time.
#
# Logs stay in /tmp. Dest files are unlinked after each step.
# Env: EWRITE_SRC, EWRITE_BIN, EWRITE_JOBS, EWRITE_SECS, EWRITE_SUBDIR
set -u
MNT=${1:?usage: ewrite_sweep.sh <mnt> <out-tsv>}
OUT=${2:?usage: ewrite_sweep.sh <mnt> <out-tsv>}

HOST=$(hostname -s)
SECS=${EWRITE_SECS:-30}
JOBS_LIST=${EWRITE_JOBS:-"2 4 8 16"}
SRC=${EWRITE_SRC:-$HOME/git/direct_rw}
BIN=${EWRITE_BIN:-/tmp/direct_rw}
SUBDIR=${EWRITE_SUBDIR:-ewrite-sweep}
LOGDIR=$(mktemp -d "/tmp/efs-ewrite-$HOST.XXXXXX")
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)

cleanup() {
    pkill -9 -x ewrite 2>/dev/null || true
    rm -rf "$LOGDIR"
}
trap cleanup EXIT

mkdir -p "$(dirname "$OUT")" "$MNT/$SUBDIR"

if [ ! -x "$BIN/ewrite" ] || [ ! -x "$BIN/ewrite.sh" ]; then
    mkdir -p "$BIN"
    rsync -a --delete --exclude='.git/' --exclude='logs/' "$SRC/" "$BIN/"
fi
if [ ! -x "$BIN/ewrite" ] || [ ! -x "$BIN/ewrite.sh" ]; then
    echo "ewrite binary missing in $BIN (and $SRC)" >&2
    exit 2
fi

sum_bytes() {
    local d=$1
    python3 -c '
import os, sys
d = sys.argv[1]
s = 0
if os.path.isdir(d):
    for n in os.listdir(d):
        p = os.path.join(d, n)
        try:
            s += os.stat(p).st_size
        except OSError:
            pass
print(s)
' "$d"
}

wipe_dir() {
    local d=$1
    python3 -c '
import os, sys
d = sys.argv[1]
if not os.path.isdir(d):
    os.makedirs(d)
    raise SystemExit
for n in os.listdir(d):
    try:
        os.unlink(os.path.join(d, n))
    except OSError:
        pass
' "$d"
}

[ -s "$OUT" ] || echo -e "ts\thost\tjobs\twall_s\tbytes\tmib_s\trc" >"$OUT"

echo "ewrite_sweep host=$HOST mnt=$MNT secs=$SECS jobs=$JOBS_LIST"

for jobs in $JOBS_LIST; do
    dest="$MNT/$SUBDIR/j$jobs"
    mkdir -p "$dest"
    wipe_dir "$dest"
    pkill -9 -x ewrite 2>/dev/null || true
    sleep 0.2

    logfile="$LOGDIR/j$jobs.out"
    t0=$(date +%s.%N)
    ( cd "$BIN" && setsid ./ewrite.sh "$dest" 1 "$jobs" \
        >"$logfile" 2>&1 </dev/null ) &
    shpid=$!

    # wait until at least one ewrite exists, or 5s
    for _ in $(seq 1 50); do
        pgrep -x ewrite >/dev/null && break
        sleep 0.1
    done

    sleep "$SECS"
    bytes=$(sum_bytes "$dest")
    t1=$(date +%s.%N)
    wall=$(python3 -c "print('%.3f' % ($t1 - $t0))")
    mib=$(python3 -c "print('%.1f' % ($bytes / 1048576.0 / $wall))" )

    pkill -9 -x ewrite 2>/dev/null || true
    wait "$shpid" 2>/dev/null || true
    # size can settle after kill; take the max of pre-kill and post-kill
    bytes2=$(sum_bytes "$dest")
    if [ "$bytes2" -gt "$bytes" ]; then
        bytes=$bytes2
        mib=$(python3 -c "print('%.1f' % ($bytes / 1048576.0 / $wall))" )
    fi

    rc=0
    nlive=$(pgrep -x ewrite | wc -l)
    [ "$nlive" -eq 0 ] || rc=1

    echo -e "$TS\t$HOST\t$jobs\t$wall\t$bytes\t$mib\t$rc" >>"$OUT"
    printf '  jobs=%-3s  wall=%ss  written=%.3f GiB  %.1f MiB/s\n' \
        "$jobs" "$wall" "$(python3 -c "print($bytes / 1073741824.0)")" "$mib"

    wipe_dir "$dest"
    rmdir "$dest" 2>/dev/null || true
done

rmdir "$MNT/$SUBDIR" 2>/dev/null || true
echo "ewrite_sweep done -> $OUT"
