#!/bin/bash
# Profile ewrite.sh 1 2 for ~PROF_SECS, then kill. Logs stay in /tmp.
# Usage: ewrite_profile_12.sh <mnt>
set -u
MNT=${1:?usage: ewrite_profile_12.sh <mnt>}
PROF_SECS=${PROF_SECS:-16}
SRC=${EWRITE_SRC:-$HOME/git/direct_rw}
BIN=${EWRITE_BIN:-/tmp/direct_rw}
OUT=${OUT:-/tmp/ew12-prof}
DEST="$MNT/ew12-prof"
HOST=$(hostname -s)

rm -rf "$OUT"
mkdir -p "$OUT" "$DEST" "$BIN"
rsync -a --delete --exclude='.git/' --exclude='logs/' "$SRC/" "$BIN/"
test -x "$BIN/ewrite" || { echo "no ewrite"; exit 2; }

python3 -c '
import os,sys
d=sys.argv[1]
os.makedirs(d,exist_ok=True)
for n in os.listdir(d):
    try: os.unlink(os.path.join(d,n))
    except OSError: pass
' "$DEST"

pkill -9 -x ewrite 2>/dev/null || true
FUSE=$(pgrep -x efs-fuse | head -1)
echo "host=$HOST fuse=$FUSE mnt=$MNT dest=$DEST" | tee "$OUT/meta.txt"

( cd "$BIN" && setsid ./ewrite.sh "$DEST" 1 2 >"$OUT/ewrite.sh.out" 2>&1 </dev/null ) &
SHPID=$!
for _ in $(seq 1 50); do
    n=$(pgrep -x ewrite | wc -l)
    [ "$n" -ge 2 ] && break
    sleep 0.1
done
EWS=$(pgrep -x ewrite | tr '\n' ' ')
echo "ewrite_pids=$EWS" | tee -a "$OUT/meta.txt"
EW1=$(echo "$EWS" | awk '{print $1}')
EW2=$(echo "$EWS" | awk '{print $2}')

# stacks + syscall + rate all overlap the same write window
timeout 14 perf record -g -F 999 -p "$FUSE" -o "$OUT/perf-fuse.data" -- sleep 12 \
    >"$OUT/perf-fuse.rec.log" 2>&1 &
[ -n "$EW1" ] && timeout 12 perf record -g -F 199 -p "$EW1" -o "$OUT/perf-ew.data" -- sleep 10 \
    >"$OUT/perf-ew.rec.log" 2>&1 &
perf stat -p "$FUSE" -- sleep 8 >"$OUT/perf-stat-fuse.txt" 2>&1 &
[ -n "$EW1" ] && perf stat -p "$EW1" -- sleep 8 >"$OUT/perf-stat-ew1.txt" 2>&1 &
if perf trace -h >/dev/null 2>&1; then
    timeout 8 perf trace -s -p "$EW1" sleep 6 >"$OUT/perf-trace-ew1.txt" 2>&1 &
    timeout 8 perf trace -s -p "$FUSE" sleep 6 >"$OUT/perf-trace-fuse.txt" 2>&1 &
fi

python3 - "$DEST" "$PROF_SECS" "$OUT/rate.txt" <<'PY'
import os,sys,time
d,secs,out=sys.argv[1],float(sys.argv[2]),sys.argv[3]
def tot():
    s=0
    if os.path.isdir(d):
        for n in os.listdir(d):
            try: s+=os.stat(os.path.join(d,n)).st_size
            except OSError: pass
    return s
t0=time.time(); a=tot(); time.sleep(secs); b=tot(); dt=time.time()-t0
open(out,"w").write("bytes0=%d bytes1=%d dt=%.3f agg_mib_s=%.1f per_stream_mib_s=%.1f\n" %
    (a,b,dt,(b-a)/dt/1048576.0,(b-a)/dt/1048576.0/2))
print(open(out).read().rstrip())
PY

( sleep 3
  {
    echo "=== ewrite wchan ==="
    for p in $EWS; do echo -n "$p "; cat /proc/$p/wchan; echo; done
    echo "=== ewrite stack ==="
    for p in $EWS; do echo "-- $p"; cat /proc/$p/stack 2>/dev/null; done
    echo "=== fuse threads ==="
    ps -p "$FUSE" -L -o tid,pcpu,stat,wchan:40,comm | sort -k2 -nr | head -25
    echo "=== fuse cpu ==="
    ps -p "$FUSE" -o pid,pcpu,nlwp,rss,comm
  } > "$OUT/snap.txt" 2>&1
) &

# Do not `wait` here: that would include ewrite.sh (hours for 100 GiB).
sleep 1

# try strace on a NEW child (same user); Yama may still deny
timeout 6 strace -c -e write,pwrite64,fdatasync,fallocate,ftruncate,fsync,openat \
    "$BIN/ewrite" "$DEST/strace-child" 8 1048576 \
    >"$OUT/strace-child.out" 2>"$OUT/strace-child.err" || true

# reports
perf report -i "$OUT/perf-fuse.data" --stdio --children -g graph,0.8,callee \
    >"$OUT/perf-fuse.report.txt" 2>/dev/null || true
perf report -i "$OUT/perf-ew.data" --stdio --no-children -n \
    >"$OUT/perf-ew.report.txt" 2>/dev/null || true

pkill -9 -x ewrite 2>/dev/null || true
wait "$SHPID" 2>/dev/null || true
python3 -c '
import os,sys
d=sys.argv[1]
if os.path.isdir(d):
    for n in os.listdir(d):
        try: os.unlink(os.path.join(d,n))
        except OSError: pass
    try: os.rmdir(d)
    except OSError: pass
' "$DEST"

echo "DONE $OUT"
cat "$OUT/rate.txt"
