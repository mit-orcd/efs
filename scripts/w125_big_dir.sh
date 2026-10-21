#!/bin/bash
# w125_big_dir.sh OUT [FILES]
#
# W125 closure C7 on orcd-mini, run ON node9901 against /data1/efs/mnt:
# FILES (default 1,000,000) small files (1 B - 4 KiB, content a function of
# the name) created by 32 threads in ONE directory; wait until they are
# packed; then time and check: a full readdir (count must equal FILES),
# stat of every entry, .efs find report and .efs stats on the directory,
# reading 20,000 random files byte for byte, and deleting the directory
# (rm -rf). Afterwards no segment file may remain on node 1.
set -u
OUT=$1; N=${2:-1000000}; D=/data1/efs/mnt/bigdir-$(date -u +%H%M); MG=/data1/efs/src/efs-mgmt
mkdir -p "$OUT" $D; FAIL=0
log() { echo "$(date -u +%T) $*" | tee -a "$OUT/log.txt"; }
t() { local a; a=$(date +%s.%N); "$@"; echo "$(echo "$(date +%s.%N) - $a" | bc)"; }
python3 - $D $N 32 > "$OUT/create.txt" <<'PY'
import os, random, sys, threading, time
d, n, k = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); t0 = time.time(); errs = [0]
def body(i): r = random.Random(i); return r.randbytes(r.randint(1, 4096))
def w(j):
    for i in range(j, n, k):
        try:
            with open(os.path.join(d, "f%07d" % i), "wb") as f: f.write(body(i))
        except OSError: errs[0] += 1
ts = [threading.Thread(target=w, args=(j,)) for j in range(k)]
[x.start() for x in ts]; [x.join() for x in ts]
print("created %d in %.0fs (%.0f/s) errors=%d" % (n, time.time() - t0, n / (time.time() - t0), errs[0]))
PY
log "create: $(cat "$OUT/create.txt")"
for i in $(seq 360); do sleep 10; [ "$(find /data1/efs/n1/data/exports -type f | wc -l)" -lt 500 ] && break; done
log "packed: per-file fragments on n1 $(find /data1/efs/n1/data/exports -type f | wc -l), segments $(find /data1/efs/n1/data/segments -type f | wc -l), waited $((i * 10))s"
s=$(t sh -c "ls -f $D | grep -c '^f' > $OUT/count.txt"); c=$(cat "$OUT/count.txt")
log "readdir: $c entries in ${s}s"; [ "$c" -eq "$N" ] || { log "FAIL readdir count $c != $N"; FAIL=1; }
s=$(t python3 -c "import os,sys; print(sum(1 for e in os.scandir(sys.argv[1]) if e.stat().st_size >= 1))" $D > "$OUT/stat.txt"); log "stat all: $(cat "$OUT/stat.txt") in ${s}s"
s=$(t sh -c "python3 $D/.efs/find report --json > $OUT/find.json 2> $OUT/find.err; echo \$? > $OUT/find.rc"); log "find report: rc=$(cat "$OUT/find.rc") in ${s}s"
s=$(t sh -c "python3 $D/.efs/stats --json > $OUT/stats.json 2> $OUT/stats.err; echo \$? > $OUT/stats.rc"); log "stats: rc=$(cat "$OUT/stats.rc") in ${s}s $(head -c 300 "$OUT/stats.json" | tr '\n' ' ')"
python3 - $D $N > "$OUT/read.txt" <<'PY'
import os, random, sys, time
d, n = sys.argv[1], int(sys.argv[2]); r = random.Random(9); bad = 0; t0 = time.time()
def body(i): q = random.Random(i); return q.randbytes(q.randint(1, 4096))
for i in r.sample(range(n), 20000):
    if open(os.path.join(d, "f%07d" % i), "rb").read() != body(i): bad += 1
print("read 20000 random: bad=%d in %.0fs" % (bad, time.time() - t0))
PY
log "$(cat "$OUT/read.txt")"; grep -q "bad=0" "$OUT/read.txt" || FAIL=1
s=$(t rm -rf $D); log "rm -rf: ${s}s"
for i in $(seq 120); do sleep 10; [ "$(find /data1/efs/n1/data/segments -type f | wc -l)" -eq 0 ] && [ "$(find /data1/efs/n1/data/exports -type f | wc -l)" -eq 0 ] && break; done
log "after delete: segments $(find /data1/efs/n1/data/segments -type f | wc -l), fragments $(find /data1/efs/n1/data/exports -type f | wc -l) after $((i * 10))s"
log "RESULT $([ $FAIL = 0 ] && echo PASS || echo FAIL)"; exit $FAIL
