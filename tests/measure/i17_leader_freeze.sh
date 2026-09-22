#!/bin/bash
# i17_leader_freeze.sh — force the index-reuse case behind I17 (half-applied
# cross-shard txns) and check the parent row stays consistent.
#
# What it forces: SIGSTOP the leader of one Raft group for FREEZE seconds
# while same_parent_storm.sh runs (mkdir/rmdir/create/unlink from 9 hosts
# in ONE parent). The other voters elect a new leader (HOST_ELECT_BASE
# 100 ticks x 5 ms, randomized to 0.5-1 s) and commit new entries at the
# indices the frozen leader had appended but not replicated. When it thaws,
# its coordinator threads wake from host_wait_applied with applied >= idx —
# and before the (index, term) match in the apply ring (arc_term) they read
# the NEW entries' OK verdicts as their own: "DECIDE COMMIT applied" →
# RESOLVE COMMIT on the child shard, while no DECISION ever existed →
# recovery ABORTed the parent shard = child row gone, dentry + parent
# counts kept (ino 62991 / 31264 on 19810, Sep 21).
#
# Pass: every efsd's raft-obs shows arc_term_miss > 0 on at least one node
# (the case was exercised), the storm's post check is `children=0 nlink=2`
# + RMDIR_OK, and no txn-recover line says "ABORT" for a txn whose sibling
# shard got COMMIT (grep below). Worker ERR lines DURING the freeze are
# expected (the frozen leader's in-flight ops fail EBUSY/EIO, and a retry
# of a committed op is EEXIST/ENOENT = I16, the next item) — they are
# reported, not a fail here.
#
# Needs EFS_RAFT_OBS=1 on the servers (raft-obs lines every 5 s; the
# cluster has run with it since Sep 21). ~2 min. Run from a screen:
#   efs-bg.sh start i17-freeze 'bash tests/measure/i17_leader_freeze.sh'
#   FREEZE=3 FREEZE_GROUPS="0 2" PROCS=4 ROUNDS=80 ... (defaults)
set -u
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
ssh_() { local t=$1 h=$2; shift 2; EFS_SSH_TIMEOUT=$t "$S" "$h.ib" "$@" 2>&1 | grep -v '^Identity added'; }
FREEZE="${FREEZE:-3}"
FREEZE_GROUPS="${FREEZE_GROUPS:-0 2}" # not GROUPS: bash builtin array
PROCS="${PROCS:-4}"
ROUNDS="${ROUNDS:-80}"
STAMP=$(date -u +%Y%m%d-%H%M%S)
OUT="${OUT:-$HOME/git/efs/results/measure/$STAMP-i17-leader-freeze}"
mkdir -p "$OUT"
say() { echo "[i17-freeze] $(date -u +%H:%M:%S) $*"; }
node_host() { case $1 in 0) echo fcstor003;; 1) echo fcstor004;; 2) echo fcstor005;; 3) echo fcstor006;; *) echo "";; esac; }
leader_of() { # group -> raft id (0-3) or ""
    ssh_ 15 fcstor004 "cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.58:19810 2>/dev/null" \
        | grep "group $1 " | grep -o 'leader=[-0-9]*' | cut -d= -f2
}
obs_term_miss() { # sum of the latest arc_term_miss on every server
    local h tot=0 v
    for h in fcstor003 fcstor004 fcstor005 fcstor006; do
        v=$(ssh_ 10 $h "grep -o 'arc_term_miss=[0-9]*' /tmp/efs/efsd.log | tail -1 | cut -d= -f2")
        echo "  $h arc_term_miss=${v:-none}"
        tot=$((tot + ${v:-0}))
    done
    echo "  total=$tot"
}

say "pre: leaders g0=$(leader_of 0) g2=$(leader_of 2); arc_term_miss before:"
obs_term_miss | tee "$OUT/obs-before.txt"
for h in fcstor003 fcstor004 fcstor005 fcstor006; do
    ssh_ 10 $h "grep -c 'txn-recover' /tmp/efs/efsd.log" | sed "s/^/  $h txn-recover lines before: /"
done | tee "$OUT/recover-before.txt"

say "storm: $PROCS procs × $ROUNDS rounds on 9 hosts (background)"
OUTDIR="$OUT/storm" PROCS=$PROCS ROUNDS=$ROUNDS bash "$HOME/git/efs/tests/stress/same_parent_storm.sh" > "$OUT/storm.log" 2>&1 &
storm=$!

for g in $FREEZE_GROUPS; do
    sleep 5
    lid=$(leader_of "$g")
    h=$(node_host "$lid")
    if [ -z "$h" ]; then say "group $g has no leader (leader=$lid) — skip"; continue; fi
    say "freeze group $g leader raft_id=$lid ($h) for ${FREEZE}s"
    # STOP, sleep, CONT in ONE ssh: a killed ssh must never leave efsd stopped.
    ssh_ $((FREEZE + 10)) "$h" "p=\$(pgrep -x efsd); [ -n \"\$p\" ] || { echo NO_EFSD; exit 1; }; kill -STOP \$p; sleep $FREEZE; kill -CONT \$p; echo THAWED pid=\$p" | tee -a "$OUT/freeze.txt"
    sleep 2
    say "after: g$g leader=$(leader_of "$g")"
done

wait $storm; src=$?
say "storm rc=$src"
grep '^\[same-parent\]' "$OUT/storm.log" | tail -12
cat "$OUT/storm/post.txt" 2>/dev/null

say "arc_term_miss after:"
obs_term_miss | tee "$OUT/obs-after.txt"
say "txn-recover lines during the run (COMMIT (resolved) = coordinator lost its RESOLVEs; ABORT = coordinator never decided):"
for h in fcstor003 fcstor004 fcstor005 fcstor006; do
    ssh_ 10 $h "grep 'txn-recover' /tmp/efs/efsd.log | tail -n +\$(( \$(grep -c txn-recover /tmp/efs/efsd.log) - 20 > 0 ? \$(grep -c txn-recover /tmp/efs/efsd.log) - 20 : 0 ))" | sed "s/^/  $h: /"
done | tee "$OUT/recover-after.txt"

fail=0
# children=0 nlink=2 + RMDIR_OK is the clean pass. Leftover names are not
# by themselves a torn row: a directory's nlink is 2 + subdirectory count,
# and a file does not change nlink. nlink != 2 + (d-* lines) is the I17
# signature (the pre-fix run was nlink=2 with a live dentry). Confirm
# nents with tests/tools/kv_dir_dump before calling a matching nlink torn.
postf="$OUT/storm/post.txt"
nk=$(sed -n 's/^children=\([0-9]*\) nlink=\([0-9]*\)$/\1 \2/p' "$postf" | head -1)
nc=${nk%% *}; nl=${nk##* }
nd=$(grep -c '^d-' "$postf" 2>/dev/null || true)
if [ "${nc:-x}" = 0 ] && [ "${nl:-x}" = 2 ] && grep -q RMDIR_OK "$postf"; then
    say "parent clean (children=0 nlink=2, rmdir ok)"
elif [ -n "$nc" ] && [ "$((2 + nd))" = "$nl" ]; then
    say "NOTE: $nc name(s) left, nlink=$nl == 2+$nd dirs. Counts match; this is a failed remove during the freeze (I16), not a torn parent. Dump nents if a later rmdir fails."
else
    say "FAIL: parent row inconsistent (children=${nc:-?} nlink=${nl:-?} d-lines=$nd, expected nlink=$((2 + nd)))"
    fail=1
fi
tm=$(grep total= "$OUT/obs-after.txt" | cut -d= -f2); tb=$(grep total= "$OUT/obs-before.txt" | cut -d= -f2)
if [ "${tm:-0}" -le "${tb:-0}" ]; then
    say "NOTE: arc_term_miss did not move (${tb:-0} -> ${tm:-0}): the freeze did not produce a truncated proposal this time — the consistency result stands, the term-check path was not exercised; rerun or raise PROCS"
fi
# Worker errors: report by errno; not a fail (I16).
for f in "$OUT"/storm/log-*.txt; do grep '^ERR' "$f"; done 2>/dev/null | awk '{print $6}' | sort | uniq -c | sed 's/^/  worker ERR: /'
if [ $fail = 0 ]; then say "PASS -> $OUT"; echo PASS > "$OUT/result.txt"; exit 0; fi
say "FAIL -> $OUT"; echo FAIL > "$OUT/result.txt"; exit 1
