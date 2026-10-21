#!/bin/bash
# bigdir_readdir_repro.sh REL OUT PORT [N] [ROUNDS] [AGE_MS]
#
# Readdir completeness of one big directory while it splits (LOCAL ->
# SPLITTING -> HASHED; the migrator runs for a long time after the burst).
# Run ON node9901: starts a private 3-daemon cluster on PORT+1..PORT+3 with
# scripts/w125_p0_cluster.sh (never orcd-mini 17432-17434 or w100
# 27432/27433), creates N files (default 500,000) in one directory with 32
# threads, then every 60 s for ROUNDS rounds (default 3) lists it with
# `ls -f` and os.listdir and records the names missing from the listdir pass.
# AGE_MS is EFS_PACK_MIN_AGE_MS (default 3600000: packing held back). PASS
# needs every round to report ls=N listdir=N missing=0. Raw cluster logs stay
# in /tmp and are removed by the final stop; OUT keeps only small result files.
set -u
REL=$1; OUT=$2; PORT=$3; N=${4:-500000}; ROUNDS=${5:-3}; AGE=${6:-3600000}
RUN=/tmp/bigdir-readdir-$PORT; FAIL=0
mkdir -p "$OUT"
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1; rm -rf "$RUN"
EFS_PACK_MIN_AGE_MS=$AGE "$REL/scripts/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" \
    > "$OUT/start.txt" 2>&1 || exit 1
D=$RUN/mnt/big; mkdir -p "$D"
python3 - "$D" "$N" <<'PY' > "$OUT/result.txt"
import os, sys, threading, time
d, n = sys.argv[1], int(sys.argv[2]); t0 = time.time()
def w(j):
    for i in range(j, n, 32):
        with open(os.path.join(d, "f%07d" % i), "wb") as f: f.write(b"x" * (1 + i % 4000))
ts = [threading.Thread(target=w, args=(j,)) for j in range(32)]; [t.start() for t in ts]; [t.join() for t in ts]
print("created %d in %.0fs" % (n, time.time() - t0))
PY
for r in $(seq "$ROUNDS"); do
  sleep 60
  a=$(ls -f "$D" | grep -c '^f')
  m=$(python3 -c "import os,sys; s=set(x for x in os.listdir(sys.argv[1]) if x.startswith('f')); m=[i for i in range(int(sys.argv[2])) if 'f%07d'%i not in s]; print(len(s), len(m), m[:20])" "$D" "$N")
  echo "round $r ls=$a listdir=${m%% *} missing=${m#* }" >> "$OUT/result.txt"
  [ "$a" = "$N" ] && [ "${m%% *}" = "$N" ] || FAIL=1
done
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1; rm -rf "$RUN"
echo "RESULT $([ $FAIL = 0 ] && echo PASS || echo FAIL)" >> "$OUT/result.txt"
exit $FAIL
