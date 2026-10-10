#!/bin/bash
set -euo pipefail
out=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats/results/measure/20261010-w92-next-three-rounds
key=/Users/mike/git/cluster/cluster_key
for ip in 111 112 113; do
 ssh -o ControlPath=none -o ConnectTimeout=10 -i "$key" -J xorinox_efs admin@192.168.122.$ip 'set -e; cd /home/admin/w92-next-three-rounds; for r in 1 2 3; do printf "round%s exit=" "$r"; cat "round$r-exit"; cat "round$r-perf.log"; sudo -n sha256sum "round$r.data"; done' > "$out/node$ip-profile-integrity.log"
 ssh -o ControlPath=none -o ConnectTimeout=10 -i "$key" -J xorinox_efs admin@192.168.122.$ip 'sudo -n bash -s' > "$out/node$ip-final-runtime.log" <<'REMOTE'
set -e
/tmp/efs/efsd --version
systemctl is-active efsd
pid=$(pgrep -x efsd)
tr '\0' ' ' < /proc/$pid/cmdline
echo
tr '\0' '\n' < /proc/$pid/environ | grep -E '^(EFS_TRANSPORT|EFS_COMMANDS|EFS_RAFT_PRESSURE|EFS_QUERY_BACKEND)='
sha256sum /tmp/efs/efsd /tmp/efs/src/meta/query_disk.c /tmp/efs/src/server/raft_host.c /tmp/efs/src/client/write.c
REMOTE

done
