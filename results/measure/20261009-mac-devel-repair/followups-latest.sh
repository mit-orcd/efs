#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-repair/latest
remote=/var/tmp/efs-mac-9c4d-validation
ssh_node_once "$GW" "sudo -n -H -u efs python3 -u /tmp/efs/tests/posix/posix_suite.py /mnt/efs-h --jobs 1 --timeout-s 600 --filter concurrent_appends --tag append-600-9c4d --keep --results $remote/append-600.tsv" > "$out/append-600.log" 2>&1 && rc=0 || rc=$?
printf 'append_600\t%s\n' "$rc" >> "$out/final-verdicts.tsv"
scp_from "$GW" "$remote/append-600.tsv" "$out/append-600.tsv"
if [ "$rc" != 0 ]; then exit "$rc"; fi
bash /tmp/efs-mac-peer-latest.sh > "$out/peer-final.log" 2>&1 && rc=0 || rc=$?
printf 'peer\t%s\n' "$rc" >> "$out/final-verdicts.tsv"
exit "$rc"
