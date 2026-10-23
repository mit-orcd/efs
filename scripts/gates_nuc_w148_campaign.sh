#!/bin/bash
# gates_nuc_w148_campaign.sh [N]
#
# W148 rerun (gates-nuc, Oct 22): N (default 9) attempts of
# w125_modify_repro.sh, 600 s each, packer minimum age 3000 ms, over TCP on
# 127.0.0.1, alternating ports 18920 (18921-18923) and 18925 (18926-18928).
# REL is $GATES/src (a `git archive` of the tested commit, built with make);
# every attempt keeps its own run directory $GATES/w125/run-bN-PORT-STAMP and
# output $GATES/w125/out-bN; one summary line per attempt goes to
# $GATES/w125/campaign.txt. EFS_CLIENT_SKIP_FOREIGN=1 lets client.sh stop
# work for a non-root user on a host that also runs another user's client
# (see scripts/client_processes.py). Raw output stays outside Git.
set -u
GATES=${GATES:-$HOME/efs-gates-nuc}
REL=$GATES/src; W=$GATES/w125; N=${1:-9}
export EFS_CLIENT_SKIP_FOREIGN=1 W125_TRANSPORT=tcp W125_ADDR=127.0.0.1 TMPDIR=$GATES/tmp
mkdir -p "$W"
for a in $(seq "$N"); do
  port=$(( a % 2 ? 18920 : 18925 ))
  stamp=$(date -u +%Y%m%dT%H%M%SZ)
  export W125_RUN=$W/run-b$a-$port-$stamp
  s=$(date +%s)
  "$REL/scripts/w125_modify_repro.sh" "$REL" "$W/out-b$a" "$port" 600 3000 > "$W/out-b$a.wrapper" 2>&1
  rc=$?
  echo "attempt=$a port=$port rc=$rc dur=$(( $(date +%s)-s ))s done=$(cat "$W/out-b$a/done" 2>/dev/null) status=$(grep -E "^final" "$W/out-b$a/a.txt" 2>/dev/null | tail -1)" >> "$W/campaign.txt"
done
echo "campaign finished $(date -u +%FT%TZ)" >> "$W/campaign.txt"
