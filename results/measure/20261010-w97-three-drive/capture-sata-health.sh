#!/bin/bash
# Read-only ATA health telemetry; no settings or tests are changed.
set -u
base=/data1/efs/w97-three-drive-v2-20261010
umask 022
end=$((SECONDS+3600))
while [ "$SECONDS" -lt "$end" ] && [ ! -e "$base/STOP-SATA" ]; do
 date -u +%Y-%m-%dT%H:%M:%SZ
 /usr/sbin/smartctl -a -j /dev/sda
 sleep 15
done > "$base/sata-health.log" 2> "$base/sata-health-errors.log"
