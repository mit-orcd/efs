#!/bin/bash
# w0a_lane_lag_repro.sh REL DIR [PORT] [SECONDS]
#
# 0a reproducer (lane fence epoch behind the inode epoch -> STALE replay that
# never converges). Starts a private 3-daemon cluster on node9901 with
# w125_p0_cluster.sh (ports PORT+1..PORT+3, default 17800 -> 17801-17803; never
# orcd-mini 17432-17434 or w100 27432/27433), mounts a second client at
# DIR/mntb, and runs scripts/w125_concurrent.py on both: client A with 4
# writers, 4 readers, 2 deleters and 4 in-place modifiers (rewrite, append,
# truncate shorter/longer, overwrite), client B --remote (readers, deleters).
# Then stops both clients and the daemons.
#
# Output (DIR/out): a.txt, b.txt (workload status and verdicts),
# stalled.txt (client publish-stalled / flush-meta EIO lines), pubstale.txt
# (efsd `pub-stale` precheck refusals, rate-limited), stale_count.txt.
# PASS: both workloads RC=0 with mismatch=0 errors=0 and stalled.txt empty.
# Before the fix this failed within 15 min (two inodes stalled, every
# pub-stale why=authority with the lane epoch one behind the inode).
# Run inside a named screen; DIR under /tmp (efsd wants 20 % free on /data1).
set -u
REL=$1; D=$2; PORT=${3:-17800}; SECS=${4:-1500}
OUT=$D/out
mkdir -p "$OUT"
"$REL/scripts/w125_p0_cluster.sh" start "$REL" "$D" "$PORT" > "$OUT/cluster.txt" 2>&1 || exit 1
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
FUSE_LIBDIR=${FUSE_LIBDIR:-/data1/efs/deps/libfuse/lib64}
[ -d "$FUSE_LIBDIR" ] && export LD_LIBRARY_PATH=$FUSE_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
mkdir -p "$D/mntb"
EFS_FUSE_LOG="$D/logs/fuseb.log" "$REL/scripts/client.sh" 10.1.99.1:$((PORT+1)) "$D/mntb" > "$D/logs/clientb.out" 2>&1
if findmnt -rn "$D/mntb" > /dev/null; then
  mkdir -p "$D/mnt/c"
  cd "$REL/scripts"
  python3 w125_concurrent.py "$D/mnt/c" "$SECS" --writers 4 --readers 4 --deleters 2 --modifiers 4 > "$OUT/a.txt" 2>&1 &
  pa=$!
  python3 w125_concurrent.py "$D/mntb/c" "$SECS" --remote --readers 4 --deleters 2 --deleted-out "$OUT/b.gone" > "$OUT/b.txt" 2>&1 &
  pb=$!
  wait $pa; echo "RC=$?" >> "$OUT/a.txt"; wait $pb; echo "RC=$?" >> "$OUT/b.txt"
else
  echo "mount B failed" >> "$OUT/cluster.txt"
fi
grep -c report-stale "$D/logs/fuse.log" > "$OUT/stale_count.txt"
grep -h "publish-stalled\|flush-meta" "$D/logs/fuse.log" "$D/logs/fuseb.log" > "$OUT/stalled.txt"
grep -h "pub-stale" "$D"/logs/efsd-n*.log > "$OUT/pubstale.txt"
"$REL/scripts/client.sh" stop "$D/mntb" > /dev/null 2>&1
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$D" > /dev/null 2>&1
# Cleanup when done: rm -rf "$D" (stores, mounts and logs).
