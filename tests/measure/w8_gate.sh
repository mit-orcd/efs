#!/bin/bash
# W8 gate, two parts, one screen.
#
# 1. Cut a one-client suite with SIGTERM and require one TSV row per test,
#    zero "[None]", and compare.py counting the rest as "not run".
# 2. Time one mkdir on each of 9 clients at once. The 9-way suite never
#    reaches its tests: its warmup (mkdir in the mount root) does not
#    return. This records which clients come back within 12 s.
#
# Remounts the same binary first and again at the end, because a hung
# warmup sits in FUSE request_wait_answer and the next run wedges on it.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
HOSTS="fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015"
MNT=/tmp/efs-mount
ssh_() { local t=$1 h=$2; shift 2; EFS_SSH_TIMEOUT=$t "$S" "$h.ib" "$@" 2>&1 | grep -v '^Identity added'; }

echo "== remount same binary"
EFS_NO_BUILD=1 bash "$REPO/tests/deploy_fuse_clients.sh" $HOSTS

echo "== push suite and cut it at 20 s on fcstor007"
ssh_ 30 fcstor007 'rsync -a --delete "$HOME/git/efs/tests/" /tmp/efs/tests/ >/dev/null
    rm -f /tmp/w8.tsv
    timeout -k 5 20 python3 /tmp/efs/tests/posix/posix_suite.py '"$MNT"' --jobs 1 --results /tmp/w8.tsv
    echo SUITE_RC=$?
    echo ROWS=$(grep -cve "^#" -e "^test" /tmp/w8.tsv)
    echo NOTRUN=$(grep -c "$(printf "\tNOTRUN\t")" /tmp/w8.tsv)
    echo PASS=$(grep -c "$(printf "\tPASS\t")" /tmp/w8.tsv)
    echo NONE=$(grep -c None /tmp/w8.tsv || true)
    grep "^# summary" /tmp/w8.tsv'
ssh_ 15 fcstor007 'cat /tmp/w8.tsv' > "$REPO/results/measure/w8-cut.tsv"
# Baseline is whatever the cut file itself contains for names that passed;
# compare a synthetic all-PASS baseline built from the TSV's test names so
# a NOTRUN row must not come out as an EFS-BUG.
awk -F'\t' 'NR>1 && $1 !~ /^#/ {print $1 "\tPASS\t"}' "$REPO/results/measure/w8-cut.tsv" > "$REPO/results/measure/w8-base.tsv"
python3 "$REPO/tests/posix/compare.py" "$REPO/results/measure/w8-base.tsv" "$REPO/results/measure/w8-cut.tsv" | tee "$REPO/results/measure/w8-compare.txt"

echo "== 9 clients mkdir in the mount root, 12 s budget each"
for h in $HOSTS; do
    ssh_ 20 "$h" "timeout 12 python3 -c 'import os,tempfile,time
t=time.time(); d=tempfile.mkdtemp(prefix=\"w8-\", dir=\"$MNT\"); print(\"MKDIR_OK %.3f\"%(time.time()-t)); os.rmdir(d)' ; echo $h rc=\$?" > "$REPO/results/measure/w8-mkdir-$h.txt" &
done
wait
for h in $HOSTS; do echo -n "$h "; tr '\n' ' ' < "$REPO/results/measure/w8-mkdir-$h.txt"; echo; done

echo "== clear any warmup left in request_wait_answer"
EFS_NO_BUILD=1 bash "$REPO/tests/deploy_fuse_clients.sh" $HOSTS
echo DONE
