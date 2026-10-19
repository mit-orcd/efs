#!/bin/bash
# w125_p5_matrix.sh REL [RUN]: P5 modification matrix on a private node9901
# cluster (ports 17681-17683): write, let the packer pack (2 s minimum age),
# modify, verify at once, let the packer repack, verify again after a remount.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); REL=$1; R=${2:-/tmp/w125-p5-run}
snap() { echo "$1 frag_files=$(find $R/n1/data/exports -type f | wc -l) seg_files=$(find $R/n1/data/segments -type f 2>/dev/null | wc -l)"; }
"$HERE/w125_p0_cluster.sh" stop "$REL" "$R" 17680 >/dev/null 2>&1; rm -rf "$R"
EFS_PACK_MIN_AGE_MS=2000 "$HERE/w125_p0_cluster.sh" start "$REL" "$R" 17680 | tail -1
python3 "$HERE/w125_p5_modify.py" "$R/mnt/m" write
sleep 30; snap packed
python3 "$HERE/w125_p5_modify.py" "$R/mnt/m" modify
python3 "$HERE/w125_p5_modify.py" "$R/mnt/m" verify; snap modified
sleep 90; snap repacked
"$REL/scripts/client.sh" stop "$R/mnt" >/dev/null 2>&1
EFS_FUSE_LOG="$R/logs/fuse2.log" "$REL/scripts/client.sh" 10.1.99.1:17681 "$R/mnt" >/dev/null 2>&1
python3 "$HERE/w125_p5_modify.py" "$R/mnt/m" verify
grep -h 'packer: group' "$R"/logs/efsd-n*.log | tail -2
"$HERE/w125_p0_cluster.sh" stop "$REL" "$R" 17680 >/dev/null 2>&1; rm -rf "$R"
