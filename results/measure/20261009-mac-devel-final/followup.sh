#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-final
remote=/var/tmp/efs-mac-devel-43fcefe190dd
record() { printf '%s\t%s\n' "$1" "$2" >> "$out/verdicts.tsv"; }
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 /var/tmp/efs-posix-remaining.py $remote/posix.tsv /mnt/efs --jobs 4 --timeout-s 30 --tag remaining-43fcefe190dd --results $remote/remaining.tsv" > "$out/remaining.log" 2>&1 && rc=0 || rc=$?
record remaining "$rc"
scp_from "$GW" "$remote/remaining.tsv" "$out/remaining.tsv"
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 tests/posix/posix_suite.py /mnt/efs --jobs 1 --timeout-s 30 --tag isolated-43fcefe190dd --filter dir_readdir_listing --results $remote/isolated.tsv" > "$out/isolated.log" 2>&1 && rc=0 || rc=$?
record isolated_readdir "$rc"
scp_from "$GW" "$remote/isolated.tsv" "$out/isolated.tsv"
for name in concurrent_create_unlink_two_proc concurrent_write_and_readdir; do
    ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 tests/posix/posix_suite.py /mnt/efs --jobs 1 --timeout-s 30 --tag retry-43fcefe190dd --filter $name --results $remote/retry-$name.tsv" > "$out/retry-$name.log" 2>&1 && rc=0 || rc=$?
    record "retry_$name" "$rc"
    scp_from "$GW" "$remote/retry-$name.tsv" "$out/retry-$name.tsv"
done
/Users/mike/git/cluster/cluster.sh status > "$out/status-final.log" 2>&1
