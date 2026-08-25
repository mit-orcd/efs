#!/bin/bash
# Timed rsync + perf of efs-fuse (and optional efsd). Run ON a client node.
# Usage: rsync_profile_70.sh [round_tag]
set -u
TAG=${1:-r}
SRC=${SRC:-$HOME/orcd/scratch/ecrawl-synt-small/}
MNT=${MNT:-/tmp/efs/mnt}
DEST="$MNT/ecrawl-$TAG"
OUT=${OUT:-/tmp/efs-rsync-prof/$TAG}
SECS=${SECS:-70}
mkdir -p "$OUT" "$DEST"

FUSE=$(pgrep -x efs-fuse | head -1)
if [ -z "$FUSE" ]; then
  echo "no efs-fuse"; exit 2
fi
echo "tag=$TAG fuse=$FUSE dest=$DEST secs=$SECS" | tee "$OUT/meta.txt"

# dest-stat first: rsync -c will checksum dest after files exist
timeout "$SECS" rsync -avvvcSP "$SRC" "$DEST/" \
  >"$OUT/rsync.out" 2>"$OUT/rsync.err" &
RPID=$!
sleep 0.3
perf record -g -F 999 -p "$FUSE" -o "$OUT/perf-fuse.data" -- sleep "$SECS" \
  >"$OUT/perf-fuse.rec.log" 2>&1 &
PERF_PID=$!

wait "$RPID" 2>/dev/null
RRC=$?
echo "rsync_rc=$RRC" | tee -a "$OUT/meta.txt"
wait "$PERF_PID" 2>/dev/null || true

python3 - "$DEST" "$OUT/counts.txt" <<'PY'
import os, sys
d, out = sys.argv[1], sys.argv[2]
nd = nf = 0
sz = 0
if os.path.isdir(d):
    for root, dirs, files in os.walk(d):
        nd += len(dirs)
        nf += len(files)
        for n in files:
            try:
                sz += os.path.getsize(os.path.join(root, n))
            except OSError:
                pass
open(out, "w").write("dirs=%d files=%d bytes=%d\n" % (nd, nf, sz))
print(open(out).read().rstrip())
PY

perf report -i "$OUT/perf-fuse.data" --stdio --no-children --percent-limit 0.8 \
  >"$OUT/perf-fuse.report.txt" 2>/dev/null || true
perf report -i "$OUT/perf-fuse.data" --stdio --children -g graph,0.8,callee \
  --percent-limit 1.5 >"$OUT/perf-fuse.children.txt" 2>/dev/null || true

echo "=== top symbols ==="
grep -E '^\s+[0-9]+\.[0-9]+%' "$OUT/perf-fuse.report.txt" | head -25
echo "DONE $OUT"
