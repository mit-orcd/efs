#!/bin/bash
# Read-only NVMe telemetry. Run as root; no device settings are changed.
set -u
base=/data1/efs/w97-three-drive-v2-20261010
mkdir -p "$base"
umask 022
/usr/sbin/nvme id-ctrl /dev/nvme0 -o json > "$base/nvme-id-ctrl.json" 2> "$base/nvme-id-errors.log"
/usr/sbin/nvme get-feature /dev/nvme0 -f 6 > "$base/nvme-write-cache.txt" 2>&1
/usr/sbin/nvme get-feature /dev/nvme0 -f 2 > "$base/nvme-power-state.txt" 2>&1
/usr/sbin/nvme get-feature /dev/nvme0 -f 12 > "$base/nvme-apst.txt" 2>&1
end=$((SECONDS+7200))
while [ "$SECONDS" -lt "$end" ] && [ ! -e "$base/STOP-NVME" ]; do
 date -u +%Y-%m-%dT%H:%M:%SZ
 /usr/sbin/nvme smart-log /dev/nvme0 -o json
 sleep 15
done > "$base/nvme-health.log" 2> "$base/nvme-health-errors.log"
