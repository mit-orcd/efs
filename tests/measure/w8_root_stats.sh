#!/bin/bash
# One client reads the mount root's stat and .stats (imm_files/imm_dirs).
# Spread starts at EFS_DIR_SPREAD_MIN (65536). A hung read sits in
# request_wait_answer; remount the same binary at the end.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-stats
ssh_ 12 fcstor007 "timeout -k 2 8 stat $MNT; echo STAT_RC=\$?
    timeout -k 2 8 head -c 500 $MNT/.stats; echo; echo STATS_RC=\$?" \
    | tee "$OUT/root-stats.txt"
remount_clients fcstor007 "" > "$OUT/remount.txt" || true
echo DONE
