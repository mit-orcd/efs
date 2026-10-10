#!/usr/bin/env bash
# One-shot private-fixture handoff, executed on the Mac in a detached screen.
set -euo pipefail
key=/Users/mike/git/cluster/cluster_key
SSH=(ssh -o ControlPath=none -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=2 -i "$key" -J xorinox_efs)
run() { "${SSH[@]}" "$1" "set -e; $2"; }
writer=/home/efs/w92-capacity-busy-fixed-20261010
while :; do
  # SSH observation failures are retried; they never restart the writer.
  if state=$(run efs@192.168.122.115 "if test -f $writer/exit-resume; then cat $writer/exit-resume; elif ps -p 269152 -o args= | grep -Fq '/home/efs/w92-query-capacity.py resume'; then echo running; else echo missing; fi"); then
    case "$state" in
      running) sleep 60; continue ;;
      0) break ;;
      *) echo "writer state $state: cold handoff refused"; exit 1 ;;
    esac
  fi
  sleep 60
done
run efs@192.168.122.115 "python3 -c 'import json; r=json.load(open(\"$writer/resume-complete.json\")); assert r[\"result\"]==\"PASS\" and r[\"phase\"]==\"resume\" and r[\"files\"]==1638400 and r[\"payload_bytes\"]==429496729600 and r[\"payloads_verified\"]+r[\"files_fsynced\"]==1638400 and r[\"snapshot\"][\"refresh_error\"]==0; print(r)'"
# Preflight every named binary and unit before stopping any private service.
for node in 111 112 113; do
  run "admin@192.168.122.$node" "test \"\$(sha256sum /tmp/efs/efsd | cut -d' ' -f1)\" = 80dc28b6285a340cb7aea802b7f6cd689df2e6f537f969aa1676ff6665585fd7 && sudo -n systemctl show w92-capacity-efsd.service -p ExecStart --value | grep -F -- '--port 18432 --storage /mnt/w92-capacity-node-20261010 --quota 250G --direct-io'"
done
run efs@192.168.122.115 'cd /tmp/efs && ./scripts/client.sh stop /home/efs/w92-capacity-mnt'
for node in 111 112 113; do
  run "admin@192.168.122.$node" 'sudo -n systemctl stop w92-capacity-efsd.service; ! sudo -n systemctl is-active --quiet w92-capacity-efsd.service'
done
echo 'all three private capacity daemons cold-stopped; preserved storage'
for id in 1 2 3; do
  node=$((110+id))
  join=''
  if [[ $id != 1 ]]; then join='--join 192.168.122.111:18432'; fi
  run "admin@192.168.122.$node" "sudo -n systemd-run --unit=w92-capacity-efsd --collect --uid=efs --gid=efs -p WorkingDirectory=/tmp/efs -p StandardOutput=append:/mnt/w92-capacity-node-20261010/efsd.log -p StandardError=inherit -E EFS_MD_RAFT_N=3 -E EFS_TRANSPORT=tcp -E EFS_QUERY_EXPORT_MAX_MS=86400000 /tmp/efs/efsd --node-id $id --addr 192.168.122.$node --port 18432 --storage /mnt/w92-capacity-node-20261010 --quota 250G --direct-io $join"
done
# Wait for recovered daemons to listen before client discovery.
for node in 111 112 113; do
  run "admin@192.168.122.$node" 'for attempt in $(seq 1 120); do if ss -lnt | grep -q ":18432 "; then exit 0; fi; sleep 2; done; echo "private daemon did not listen within 240 seconds" >&2; exit 1'
done
run efs@192.168.122.115 'cd /tmp/efs && EFS_COMMANDS=1 EFS_OPID_TRACE=1 ./scripts/client.sh 192.168.122.111:18432 /home/efs/w92-capacity-mnt'
# The actual complete verifier owns a remote screen, independent of this SSH.
run efs@192.168.122.115 'test ! -e /home/efs/w92-capacity-cold-verify-20261010 && mkdir /home/efs/w92-capacity-cold-verify-20261010 && screen -dmS w92-capacity-cold-verify-20261010 bash -c '\''python3 /home/efs/w92-query-capacity.py verify /home/efs/w92-capacity-mnt w92-real-400g-20261010 /home/efs/w92-capacity-cold-verify-20261010 > /home/efs/w92-capacity-cold-verify-20261010/verify.log 2>&1; status=$?; printf "%s\n" "$status" > /home/efs/w92-capacity-cold-verify-20261010/exit-verify; date -u > /home/efs/w92-capacity-cold-verify-20261010/finished-verify; exit "$status"'\'''
echo 'fresh named client mounted; full cold payload/namespace/export verifier queued'
