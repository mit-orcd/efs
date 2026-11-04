#!/bin/bash
# Independent Intel-export client on amd_efs or the physical xorinox_efs host.
# Never starts/stops host cluster daemons or sources a cluster's configuration.
set -euo pipefail
client_base=/data1/efs/intel-client
client_mount=/data1/efs/intel-mnt
client_seed=10.1.0.208:17432
case "${1:-start}" in
    start)
        [ "$(id -un)" = efs ] && [ "$(id -u)" -ne 0 ] || {
            echo 'Run the Intel client as the efs account' >&2; exit 1;
        }
        mkdir -p "$client_base/logs"
        exec env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 \
            EFS_EXPIRY=0 \
            EFS_FUSE_LOG="$client_base/logs/efs-fuse-intel-mnt.log" \
            "$client_base/src/scripts/client.sh" "$client_seed" "$client_mount"
        ;;
    stop)
        exec "$client_base/src/scripts/client.sh" stop "$client_mount"
        ;;
    status)
        findmnt -rn -M "$client_mount" -o TARGET,SOURCE,FSTYPE,OPTIONS
        exec "$client_base/src/efs-mgmt" status "$client_seed"
        ;;
    *)
        echo "Usage: $0 [start|stop|status]" >&2; exit 2
        ;;
esac
