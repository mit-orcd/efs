#!/bin/bash
# ior_hard_scaling.sh — W6 residual 1: ior-hard-write is 45 MiB/s with 36
# ranks (47008 B records, all ranks interleaved in ONE file, so every
# 128 KiB chunk is shared by ~3 ranks and every fsync replays the CAS
# losers). How does it scale with rank count — is the wall the N-1 CAS, or
# the same per-client ceiling W4 sees?
#
# For each NP in NPS: run the raw IOR hard geometry
# (tests/perf/io500/run.sh ior-hard-write <segs>, detached prterun on
# RANK0, -k keeps the file), poll RANK0's last-run.log until IOR prints its
# summary, record "Max Write" MiB/s and the wall, delete the file. Rank0's
# client runs under EFS_RPC_PROF=1 so the report/BUSY counters are captured
# too (remounted per NP).
#
# Usage (from node9901 via efs-bg.sh; 36 ranks x 3000 segs = 4.7 GiB at
# ~45 MiB/s is ~2 min; the whole default set ~8 min):
#   bash tests/measure/ior_hard_scaling.sh
#   NPS="1 4 9 36" SEGS=3000 bash tests/measure/ior_hard_scaling.sh
# Prereq: tests/perf/io500/run.sh prepare has been run once (bin/ior exists
# under $IO500_DIR on the NFS home).
#
# Hand the user: MiB/s vs NP, the RPC-PROF report count / busy_n / recv_us
# for rank0's client, and the per-rank bytes (segs*47008). A flat or
# falling curve past 9 ranks with busy_n growing = CAS replay; flat with
# busy_n ~0 = the W4 write wall. The designed escape is §9 immutable delta
# objects — a user decision, not something to build here.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
NPS="${NPS:-1 4 9 36}"
SEGS="${SEGS:-3000}"
RANK0="${RANK0:-fcstor007}"
IO500_DIR="${IO500_DIR:-$HOME/orcd/scratch/efs-io500}"
HARD_FILE="$MNT/io500/hardv/file"
mkout ior-hard-scaling
preflight_or_die
[ -x "$IO500_DIR/io500/bin/ior" ] || [ -x "$IO500_DIR/bin/ior" ] || say "WARNING: no ior binary under $IO500_DIR — run tests/perf/io500/run.sh prepare first"

printf 'np\tslots\tsegs\tbytes_total_mib\tlaunch_wall_s\tior_write_s\twrite_mibs\tbusy_n\tbusy_s\n' > "$OUT/table.tsv"
for np in $NPS; do
    slots=$(( (np + 8) / 9 )); [ $slots -lt 1 ] && slots=1
    say "== NP=$np SLOTS=$slots SEGS=$SEGS"
    remount_clients "$RANK0" "EFS_RPC_PROF=1" > "$OUT/remount-$np.txt" || { cat "$OUT/remount-$np.txt"; exit 1; }
    ssh_ 30 "$RANK0" "rm -f $HARD_FILE; mkdir -p $(dirname $HARD_FILE)" >/dev/null
    : > "$IO500_DIR/last-run.log"
    srv_log_mark
    t0=$(date +%s)
    SLOTS=$slots NP=$np RANK0="$RANK0.ib" bash "$REPO/tests/perf/io500/run.sh" ior-hard-write "$SEGS" | tee "$OUT/launch-$np.txt"
    # IOR ends with a "Finished" line; the driver is detached, so poll the NFS log.
    deadline=$(( t0 + 1500 )); done_=0
    while [ "$(date +%s)" -lt $deadline ]; do
        grep -qaE '^Finished|^Max Write|ior ERROR|ERROR:' "$IO500_DIR/last-run.log" 2>/dev/null && { done_=1; break; }
        sleep 5
    done
    wall=$(( $(date +%s) - t0 ))
    cp "$IO500_DIR/last-run.log" "$OUT/ior-$np.log"
    [ $done_ = 1 ] || { say "FAIL: IOR NP=$np not finished after ${wall}s — check for D-state ranks (pgrep -x ior on the clients; pkill -9 -x ior, remount). Not raising the deadline."; }
    # single-phase IOR prints no "Max Write:" — the per-test row is
    #   write  <bw MiB/s>  <IOPS> ...
    mw=$(grep -a 'Max Write' "$OUT/ior-$np.log" | grep -oE '[0-9.]+ MiB/sec' | head -1 | cut -d' ' -f1)
    [ -n "$mw" ] || mw=$(grep -aE '^write +[0-9]' "$OUT/ior-$np.log" | head -1 | awk '{print $2}')
    iowall=$(grep -aE '^write +[0-9]' "$OUT/ior-$np.log" | head -1 | awk '{print $10}')
    line=$(rpc_prof_last "$RANK0"); echo "$line" > "$OUT/rpcprof-$np.txt"
    bn=$(n0 "$(rpc_field "$line" busy_n)"); bus=$(n0 "$(rpc_field "$line" busy_us)")
    rs=$(srv_report_split "$OUT/report-split-$np.txt"); echo "NP=$np: $rs" >> "$OUT/report-split-summary.txt"
    printf '%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%s\n' "$np" "$slots" "$SEGS" $(( np * SEGS * 47008 / 1048576 )) "$wall" "${iowall:-?}" "${mw:-FAIL}" "$bn" "$(echo "scale=1; $bus/1000000" | bc)" >> "$OUT/table.tsv"
    say "NP=$np: write ${mw:-FAIL} MiB/s (IOR wall ${iowall:-?}s, launch-to-finish ${wall}s); rank0 client busy_n=$bn; $rs"
done
ssh_ 30 "$RANK0" "rm -f $HARD_FILE" >/dev/null
remount_clients "$RANK0" "" > "$OUT/remount-restore.txt" || cat "$OUT/remount-restore.txt"
{
    echo "ior-hard-write scaling, build=$BUILD, $(date -u +%F): -t 47008 -b 47008 -s $SEGS, one shared file, ranks spread over $RANK0.. (SLOTS per host)"
    column -t -s $'\t' "$OUT/table.tsv"
    echo; echo "server REPORT timing (efsd report-split lines during each run):"; cat "$OUT/report-split-summary.txt"
    echo
    echo "busy_n/busy_s are rank0's CLIENT only (1/np of the ranks at SLOTS=1, 4/np at SLOTS=4) and exclude REPORT (unprofiled on this build). Raw: ior-*.log rpcprof-*.txt report-split-*.txt"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
