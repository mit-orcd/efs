#!/bin/bash
# mkdir_hammer.sh — 9 clients × N processes, each mkdir'ing into its own
# directory as fast as it can. This is the 9-host posix shape (16 jobs per
# client, each test its own parent), not the same-parent storm.
#
# Usage (screen on node9901):
#   bash tests/measure/mkdir_hammer.sh
#   PROCS=16 SECS=15 bash tests/measure/mkdir_hammer.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
PROCS="${PROCS:-16}"
SECS="${SECS:-15}"
IDLE_SECS="${IDLE_SECS:-5}"
WORKER="$REPO/tests/stress/mkdir_hammer_worker.py"
mkout mkdir-hammer
preflight_or_die
srv_log_mark

run_level() {
    local label=$1 secs=$2 procs=$3 hosts=$4
    local parent="$MNT/mhammer-$STAMP-$label"
    local first
    first=$(echo $hosts | awk '{print $1}')
    say "== $label procs=$procs secs=$secs hosts=[$hosts]"
    ssh_ 15 "$first" "mkdir $parent && echo PARENT_OK" | grep -q PARENT_OK \
        || { say "FAIL: mkdir $parent"; exit 2; }
    local i=0 h
    for h in $hosts; do
        ssh_ 90 "$h" "PYTHONUNBUFFERED=1 python3 $WORKER $parent $procs $secs $h" \
            > "$OUT/log-$label-$h.txt" 2>&1 &
        eval "wpid_$i=$!"; i=$((i + 1))
    done
    local fail=0
    i=0
    for h in $hosts; do
        eval "wait \$wpid_$i"; local rc=$?
        if [ $rc != 0 ] || ! grep -q '^HOST_DONE' "$OUT/log-$label-$h.txt"; then
            say "FAIL $label $h rc=$rc"
            fail=$((fail + 1))
        fi
        i=$((i + 1))
    done
    python3 - "$label" "$OUT" $hosts <<'PY'
import sys
label, out = sys.argv[1], sys.argv[2]
hosts = sys.argv[3:]
rows = []
errs = 0
for h in hosts:
    for line in open("%s/log-%s-%s.txt" % (out, label, h)):
        if line.startswith("ERR"):
            errs += 1
        if not line.startswith("DONE "):
            continue
        f = {}
        for tok in line.split():
            if "=" not in tok:
                continue
            k, v = tok.split("=", 1)
            f[k] = float(v)
        f["host"] = h
        rows.append(f)
n = int(sum(r["n"] for r in rows))
p50 = sorted(r["p50"] for r in rows)
p99 = sorted(r["p99"] for r in rows)
mx = max((r["max"] for r in rows), default=0)
def med(xs):
    return xs[len(xs)//2] if xs else 0
print("%s procs=%d mkdir=%d err_lines=%d p50_med=%.2f p50_worst=%.2f p99_med=%.2f p99_worst=%.2f max=%.2f"
      % (label, len(rows), n, errs, med(p50), p50[-1] if p50 else 0, med(p99), p99[-1] if p99 else 0, mx))
open("%s/summary-%s.txt" % (out, label), "w").write(
    "procs=%d mkdir=%d err_lines=%d p50_med=%.2f p50_worst=%.2f p99_med=%.2f p99_worst=%.2f max=%.2f\n"
    % (len(rows), n, errs, med(p50), p50[-1] if p50 else 0, med(p99), p99[-1] if p99 else 0, mx))
PY
    ssh_ 20 "$first" "leftover=\$(find $parent -mindepth 1 -maxdepth 1 2>/dev/null | wc -l); rmdir $parent 2>/dev/null; echo leftover=\$leftover; echo rmdir_rc=\$?" \
        | tee "$OUT/clean-$label.txt"
    [ $fail = 0 ] || say "WARNING: $label had $fail host failures"
}

t0=$(date +%s)
run_level idle "$IDLE_SECS" 1 "fcstor007"
run_level hammer "$SECS" "$PROCS" "$ALL_CLIENTS"
wall=$(( $(date +%s) - t0 ))

say "raft-obs since the mark"
: > "$OUT/obs.txt"
for h in $SERVERS; do
    echo "== $h" >> "$OUT/obs.txt"
    ssh_ 15 "$h" "tail -n +$(( ${SRV_MARK[$h]:-0} + 1 )) /tmp/efs/efsd.log | grep 'raft-obs: wait_timeouts'" >> "$OUT/obs.txt" || true
done
python3 - "$OUT/obs.txt" "$OUT/obs-peak.txt" <<'PY'
import re, sys
host = "?"
best = {}
for line in open(sys.argv[1]):
    if line.startswith("== "):
        host = line.split()[1]
        continue
    if "apply_max=" not in line:
        continue
    def g(k, line=line):
        m = re.search(k + r"=(\d+)", line)
        return int(m.group(1)) if m else 0
    rec = {k: g(k) for k in
           ("apply_max", "persist_max", "pump_hold_max", "applies_in_worst",
            "wait_timeouts", "fin_q", "fin_done")}
    rec["host"] = host
    for k in ("apply_max", "persist_max", "pump_hold_max"):
        if rec[k] >= best.get(k, {}).get(k, -1):
            best[k] = rec
lines = []
for k, rec in best.items():
    lines.append("%s host=%s apply_max=%d us persist_max=%d us pump_hold_max=%d us applies_in_worst=%d wait_timeouts=%d fin_q=%d fin_done=%d"
                 % (k, rec["host"], rec["apply_max"], rec["persist_max"], rec["pump_hold_max"],
                    rec["applies_in_worst"], rec["wait_timeouts"], rec["fin_q"], rec["fin_done"]))
text = "\n".join(lines) + ("\n" if lines else "no raft-obs lines\n")
open(sys.argv[2], "w").write(text)
print(text, end="")
PY

{
    echo "mkdir hammer build=$BUILD wall=${wall}s procs=$PROCS secs=$SECS $(date -u +%FT%TZ)"
    echo "idle (1 proc, fcstor007, ${IDLE_SECS}s): $(cat "$OUT/summary-idle.txt")"
    echo "hammer ($PROCS x 9, ${SECS}s, own dir each): $(cat "$OUT/summary-hammer.txt")"
    echo
    echo "obs peaks (us):"
    cat "$OUT/obs-peak.txt"
} | tee "$OUT/SUMMARY.txt"
say "done $OUT"
