#!/bin/bash
# w125_p6_landing.sh REL [BASE_REL] [RUN]
#
# W125 P6 behavioural landing checks on a private node9901 cluster (ports
# 17691-17693; never orcd-mini or w100), checked through persisted mappings
# (efs-mgmt raft-getchunks) and the physical fragment inventory of node 1:
#   L1 fresh small files are packed after the minimum age and read back;
#   L2 their per-file fragments and inode directories are retired;
#   L3 a large file keeps per-file chunks;
#   L4 empty files create no chunk row and no segment record;
#   L5 a modified packed file is rewritten per file and repacked;
#   L6 (with BASE_REL) small files written by the base build are backfilled
#      and packed after an upgrade;
#   L7 no tunable turns the packer off: with every tunable at 0 it still packs.
# Prints one PASS/FAIL line per check and exits non-zero on any FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); REL=$1; BASE=${2:-}; R=${3:-/tmp/w125-p6-landing}
PORT=17690; ADDR=10.1.99.1:$((PORT+1)); FAIL=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAIL=1; }
inv() { find $R/n1/data/exports -type f 2>/dev/null | wc -l; }
idir() { local ino=$1; find $R/n1/data/exports -type d -path "*/$(printf '%04d/%04d' $(((ino/10000)%10000)) $((ino%10000)))" 2>/dev/null | wc -l; }
packed() { "$REL/efs-mgmt" raft-getchunks $ADDR "$1" 2>/dev/null | grep -c 'packed segment='; }
rows() { "$REL/efs-mgmt" raft-getchunks $ADDR "$1" 2>/dev/null | sed -n 's/.* count=\([0-9]*\).*/\1/p'; }
start() { "$HERE/w125_p0_cluster.sh" stop "$1" $R $PORT >/dev/null 2>&1; [ "${3:-}" = keep ] || rm -rf $R
          env $2 "$HERE/w125_p0_cluster.sh" start "$1" $R $PORT | tail -1; }
stop() { "$HERE/w125_p0_cluster.sh" stop "$1" $R $PORT >/dev/null 2>&1; }
wait_inv() { for t in $(seq 120); do [ "$(inv)" -le "$1" ] && return 0; sleep 1; done; return 1; }

start "$REL" EFS_PACK_MIN_AGE_MS=2000
M=$R/mnt
head -c 1048576 /dev/urandom > $M/big; BIG=$(stat -c %i $M/big)
python3 "$HERE/w125_p3_readback.py" write $M/small 500 >/dev/null
for i in $(seq 50); do : > $M/empty$i; done; EMPTY=$(stat -c %i $M/empty7)
BIGFRAGS=8 # 1 MiB = 8 chunks of 128 KiB, one fragment each on node 1
S1=$(stat -c %i $M/small/d000/f000011)
if wait_inv $BIGFRAGS && [ "$(packed $S1)" = 1 ] && python3 "$HERE/w125_p3_readback.py" verify $M/small 500 | grep -q 'bad=0'
then pass "L1 500 small files packed (inventory $(inv), mapping packed) and read back"; else fail "L1 inventory=$(inv) packed=$(packed $S1)"; fi
[ "$(idir $S1)" = 0 ] && pass "L2 per-file fragments and inode directory of a packed file retired" || fail "L2 inode dirs=$(idir $S1)"
[ "$(packed $BIG)" = 0 ] && [ "$(rows $BIG)" = 8 ] && pass "L3 large file keeps 8 per-file chunks" || fail "L3 rows=$(rows $BIG) packed=$(packed $BIG)"
[ "$(rows $EMPTY)" = 0 ] && pass "L4 empty files have no chunk row" || fail "L4 rows=$(rows $EMPTY)"
for i in 0 1 2 3 4 5 6 7 8 9; do printf 'rewritten %s\n' $i > $M/small/d000/f00000$i; done
S0=$(stat -c %i $M/small/d000/f000000)
M5=$(inv)
if [ "$M5" -gt $BIGFRAGS ] && [ "$(packed $S0)" = 0 ] && wait_inv $BIGFRAGS && [ "$(packed $S0)" = 1 ] &&
   [ "$(cat $M/small/d000/f000000)" = "rewritten 0" ]
then pass "L5 rewritten packed files went per-file ($M5 fragments) and were repacked"; else fail "L5 inventory=$(inv) packed=$(packed $S0)"; fi
stop "$REL"

if [ -n "$BASE" ]; then
  start "$BASE" EFS_PACK_MIN_AGE_MS=0
  python3 "$HERE/w125_p3_readback.py" write $M/legacy 300 >/dev/null
  stop "$BASE"
  start "$REL" EFS_PACK_MIN_AGE_MS=2000 keep
  if wait_inv 0 && python3 "$HERE/w125_p3_readback.py" verify $M/legacy 300 | grep -q 'bad=0'
  then pass "L6 300 base-build files backfilled, packed and read back"; else fail "L6 inventory=$(inv)"; fi
  stop "$REL"
else
  echo "SKIP L6 (no BASE_REL)"
fi

start "$REL" "EFS_PACK_MIN_AGE_MS=0 EFS_PACK_FILES_PER_S=0 EFS_PACK_BUSY_FILES_PER_S=0 EFS_PACK_BUSY_COMMITS_PER_S=0 EFS_PACK_SEGMENTS_PER_PASS=0 EFS_PACK_INTERVAL_MS=0 EFS_PACK_BACKFILL_SHARDS=0"
python3 "$HERE/w125_p3_readback.py" write $R/mnt/z 5 >/dev/null
if wait_inv 0; then pass "L7 every tunable at 0: the packer still packs"; else fail "L7 inventory=$(inv)"; fi
grep -qiE 'getenv\("EFS_PACK_[A-Z_]*(DISABLE|OFF|ENABLE)' "$HERE/../src/server/packer.c" &&
  fail "L7 packer.c reads an enable/disable variable" || pass "L7 packer.c has no enable/disable variable"
stop "$REL"; rm -rf $R
[ $FAIL = 0 ] && echo "landing: PASS" || echo "landing: FAIL"
exit $FAIL
