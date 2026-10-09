#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-user.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-final
remote=/var/tmp/efs-mac-devel-43fcefe190dd
record() { printf '%s\t%s\n' "$1" "$2" >> "$out/verdicts.tsv"; }
/Users/mike/git/cluster/cluster.sh start > "$out/start.log" 2>&1
ssh_node_once "$GW" "sudo -n -H -u efs mkdir -p $remote"
ssh_node_once "$GW" "set -e; baseline=\$(sudo -n -H -u efs mktemp -d /var/tmp/efs-mac-baseline.XXXXXX); cd /tmp/efs; set +e; sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 tests/posix/posix_suite.py \"\$baseline\" --jobs 4 --timeout-s 30 --results $remote/baseline.tsv; rc=\$?; true; exit 0" > "$out/baseline.log" 2>&1
ssh_node_once "$GW" "cd /tmp/efs; sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 tests/posix/posix_suite.py /mnt/efs --jobs 4 --timeout-s 30 --tag devel-43fcefe190dd --results $remote/posix.tsv" > "$out/posix.log" 2>&1 && rc=0 || rc=$?
record posix "$rc"
scp_from "$GW" "$remote/baseline.tsv" "$out/baseline.tsv"
scp_from "$GW" "$remote/posix.tsv" "$out/posix.tsv"
python3 "$EFS_SRC/tests/posix/compare.py" "$out/baseline.tsv" "$out/posix.tsv" > "$out/compare.log" 2>&1 && rc=0 || rc=$?
record baseline_compare "$rc"
prepare_client_mount "$GW" /mnt/efs-b
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh $(ip_of "${NODES[0]}"):$EFS_PORT /mnt/efs-b -o allow_other" > "$out/peer-mount.log" 2>&1
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 bash -c 'python3 tests/posix/posix_2client.py --prepare /mnt/efs --parent posix-2c-43fcefe190dd && POSIX2_STEP_SEC=45 python3 tests/posix/posix_2client.py --remote mount-a mount-b --mnt /mnt/efs --ssh /var/tmp/efs-two-mount-exec.py --parent posix-2c-43fcefe190dd --results $remote/posix2.tsv'" > "$out/posix2.log" 2>&1 && rc=0 || rc=$?
record posix2 "$rc"
scp_from "$GW" "$remote/posix2.tsv" "$out/posix2.tsv"
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 timeout --kill-after=5s 180s python3 tests/posix/posix_2client.py --local /mnt/efs /mnt/efs-b --parent timestamp-43fcefe190dd --filter peer_dir_mtime_bump_visible --results $remote/timestamp-local.tsv" > "$out/timestamp-local.log" 2>&1 && rc=0 || rc=$?
record timestamp_local "$rc"
scp_from "$GW" "$remote/timestamp-local.tsv" "$out/timestamp-local.tsv"
stop_client "$GW" /mnt/efs-b > "$out/peer-stop.log" 2>&1
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 tests/posix/posix_persist.py /mnt/efs --phase prepare --tag devel-43fcefe190dd --timeout-s 60 --results $remote/persist-prepare.tsv" > "$out/persist-prepare.log" 2>&1 && rc=0 || rc=$?
record persist_prepare "$rc"
# The NFS kernel export cache can retain the otherwise idle FUSE mount.
ssh_node_once "$GW" 'sudo -n exportfs -u "*:/mnt/efs" && sudo -n exportfs -f' > "$out/persist-export-withdraw.log" 2>&1
if stop_client "$GW" /mnt/efs > "$out/persist-stop.log" 2>&1; then true; else
    ssh_node_once "$GW" 'sudo -n exportfs -r'
    exit 1
fi
/Users/mike/git/cluster/gw-up.sh > "$out/persist-remount.log" 2>&1
ssh_node_once "$GW" "cd /tmp/efs && sudo -n -H -u efs env PYTHONUNBUFFERED=1 python3 tests/posix/posix_persist.py /mnt/efs --phase verify --tag devel-43fcefe190dd --timeout-s 60 --results $remote/persist-verify.tsv" > "$out/persist-verify.log" 2>&1 && rc=0 || rc=$?
record persist_verify "$rc"
scp_from "$GW" "$remote/persist-prepare.tsv" "$out/persist-prepare.tsv"
scp_from "$GW" "$remote/persist-verify.tsv" "$out/persist-verify.tsv"
/Users/mike/git/cluster/cluster.sh status > "$out/status.log" 2>&1
for node in "${NODES[@]}" "$GW"; do
    ssh_node_once "$node" 'ps -C efsd -C efs-fuse -o user,pid,args; cd /tmp/efs; sudo -n -H -u efs ./efsd --version; sha256sum efsd efs-fuse' > "$out/installed-$node.txt"
done
