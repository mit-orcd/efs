#!/bin/bash
# same_parent_storm.sh — many procs on many clients mkdir/rmdir/create/unlink
# in ONE shared parent, then check the parent's link count and that it can
# be removed.
#
# Reproduces the Sep 20 mdtest "wedged directory": concurrent same-parent
# mkdir/rmdir from 36 ranks left test-dir.0-0 at nlink=2 with 3 children;
# every later rmdir was EIO forever (parent-row lost update between the
# unversioned log-path PUT and the txn EXCL full-image CAS; fixed by §7.2
# commutative REDUCE parts). Pass: after every worker finished, the parent
# is empty, `stat` nlink == 2, and `rmdir` succeeds. Not an XFS-compare.
#
# Usage (from node9901 via efs-bg.sh; FUSE mounted on every host):
#   tests/stress/same_parent_storm.sh
#   PROCS=8 ROUNDS=200 HOSTS="fcstor007.ib fcstor008.ib" tests/stress/same_parent_storm.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
PROCS="${PROCS:-4}"      # per host
ROUNDS="${ROUNDS:-100}"  # mkdir+rmdir (+create+unlink) per proc
HOSTS="${HOSTS:-fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib}"
STAMP=$(date -u +%Y%m%d-%H%M%S)
PARENT="$MNT/spstorm-$STAMP"
OUTDIR="${OUTDIR:-$HOME/git/efs/results/stress/same-parent-$STAMP}"
mkdir -p "$OUTDIR"
say() { echo "[same-parent] $*"; }

first=; ncli=0
for h in $HOSTS; do [ -n "$first" ] || first=$h; ncli=$((ncli + 1)); done
say "clients=$ncli procs=$PROCS rounds=$ROUNDS parent=$PARENT -> $OUTDIR"

for h in $HOSTS; do
    EFS_SSH_TIMEOUT=10 $SSH "$h" "findmnt -no FSTYPE $MNT; stat $MNT/ >/dev/null && echo MOUNT_OK" 2>/dev/null \
        | grep -q MOUNT_OK || { say "FAIL: $h mount not usable"; exit 2; }
done
EFS_SSH_TIMEOUT=10 $SSH "$first" "mkdir $PARENT && echo PARENT_OK" | grep -q PARENT_OK || { say "FAIL: mkdir parent"; exit 2; }

# Worker (same_parent_worker.py): PROCS forked pythons, each ROUNDS times
# mkdir d / create f / rmdir d / unlink f in the parent. Names are unique
# per (host,proc,round), so every conflict is on the PARENT row / dseq
# witness, not on a dentry. The NFS $HOME copy is what each host runs.
WORKER="$HOME/git/efs/tests/stress/same_parent_worker.py"
i=0
for h in $HOSTS; do
    short=${h%.ib}
    EFS_SSH_TIMEOUT=600 $SSH "$h" "PYTHONUNBUFFERED=1 python3 $WORKER $PARENT $PROCS $ROUNDS $short" \
        > "$OUTDIR/log-$short.txt" 2>&1 &
    eval "wpid_$i=$!"; i=$((i + 1))
done
fail=0; i=0
for h in $HOSTS; do
    short=${h%.ib}
    eval "wait \$wpid_$i"; rc=$?
    nerr=$(grep -c '^ERR' "$OUTDIR/log-$short.txt")
    if [ $rc = 0 ] && grep -q '^HOST_DONE' "$OUTDIR/log-$short.txt" && [ "$nerr" = 0 ]; then
        say "PASS $short $(grep -c '^DONE' "$OUTDIR/log-$short.txt") procs, slowest $(grep '^DONE' "$OUTDIR/log-$short.txt" | sed 's/.*secs=//' | sort -n | tail -1)s"
    else
        say "FAIL $short rc=$rc errs=$nerr $(grep '^ERR' "$OUTDIR/log-$short.txt" | awk '{print $6}' | sort | uniq -c | tr '\n' ' ')"
        fail=$((fail + 1))
    fi
    i=$((i + 1))
done

# The check that mattered: parent empty, nlink 2, removable.
post=$(EFS_SSH_TIMEOUT=30 $SSH "$first" "n=\$(ls -A $PARENT | wc -l); nl=\$(stat -c %h $PARENT); echo children=\$n nlink=\$nl; rmdir $PARENT && echo RMDIR_OK || echo RMDIR_FAIL; ls -A $PARENT 2>/dev/null | head -5")
echo "$post" | tee "$OUTDIR/post.txt"
echo "$post" | grep -q '^children=0 nlink=2$' || { say "FAIL: parent not clean (expected children=0 nlink=2)"; fail=$((fail + 1)); }
echo "$post" | grep -q RMDIR_OK || { say "FAIL: rmdir parent"; fail=$((fail + 1)); }

if [ $fail = 0 ]; then
    say "PASS $ncli×$PROCS procs × $ROUNDS rounds in one parent"
    echo PASS > "$OUTDIR/result.txt"; exit 0
fi
say "FAIL ($fail) — see $OUTDIR"
echo FAIL > "$OUTDIR/result.txt"; exit 1
