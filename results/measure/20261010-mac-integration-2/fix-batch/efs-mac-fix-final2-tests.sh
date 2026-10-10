#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261010-mac-integration-2/fix-batch
mkdir -p "$out"
remote=/var/tmp/efs-mac-integration2-fixbatch-20261010
record() { printf '%s\t%s\n' "$1" "$2" >> "$out/final-verdicts.tsv"; }
ssh_node_once "$GW" "sudo -n -H -u efs mkdir -p $remote"
ssh_node_once "$GW" "sudo -n -H -u efs bash -c 'base=\$(mktemp -d /var/tmp/efs-native-core.XXXXXX); python3 -u /tmp/efs/tests/posix/posix_suite.py \"\$base\" --jobs 4 --timeout-s 30 --results $remote/native-core.tsv'" > "$out/native-core-final.log" 2>&1 && rc=0 || rc=$?
record native_core "$rc"
scp_from "$GW" "$remote/native-core.tsv" "$out/native-core-final.tsv"
ssh_node_once "$GW" "sudo -n -H -u efs python3 -u /tmp/efs/tests/posix/posix_suite.py /mnt/efs --jobs 1 --timeout-s 600 --tag integration2-fixbatch-20261010 --keep --results $remote/posix.tsv" > "$out/posix-mac-profile.log" 2>&1 && rc=0 || rc=$?
record posix "$rc"
scp_from "$GW" "$remote/posix.tsv" "$out/posix-mac-profile.tsv"
ssh_node_once "$GW" "sudo -n -H -u efs python3 -u /tmp/efs/tests/posix/posix_persist.py /mnt/efs --phase prepare --keep --tag integration2-fixbatch-20261010 --timeout-s 600 --results $remote/persist-prepare.tsv" > "$out/persist-prepare-final.log" 2>&1 && rc=0 || rc=$?
record persist_prepare "$rc"
scp_from "$GW" "$remote/persist-prepare.tsv" "$out/persist-prepare-final.tsv"
ssh_node_once "$GW" 'sudo -n exportfs -u "*:/mnt/efs"; sudo -n exportfs -f' > "$out/export-withdraw.log" 2>&1
trap 'ssh_node_once "$GW" "sudo -n exportfs -r"' EXIT
if stop_client "$GW" /mnt/efs > "$out/persist-stop-final.log" 2>&1; then
    record clean_remount 0
    ssh_node_once "$GW" 'cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other' > "$out/persist-remount-final.log" 2>&1
    ssh_node_once "$GW" "sudo -n -H -u efs python3 -u /tmp/efs/tests/posix/posix_persist.py /mnt/efs --phase verify --keep --tag integration2-fixbatch-20261010 --timeout-s 600 --results $remote/persist-verify.tsv" > "$out/persist-verify-final.log" 2>&1 && rc=0 || rc=$?
    record persist_verify "$rc"
    scp_from "$GW" "$remote/persist-verify.tsv" "$out/persist-verify-final.tsv"
else
    record clean_remount 1
fi
