# NFS holds the gateway's FUSE mount busy. Withdraw only the EFS export;
# restore configured exports if client stop fails, keeping servers available.
stop_client() {  # VM, mountpoint, optional --force-discard
    local remote_stop mount_q
    printf -v mount_q '%q' "$2"
    printf -v remote_stop 'cd /tmp/efs && sudo -n -H -u efs ./scripts/client.sh stop %s%s' "${3:+$3 }" "$mount_q"
    if [ "$1" = "$GW" ] && [ "$2" = /mnt/efs ]; then
        remote_stop="set -eo pipefail
            exports=\$(sudo exportfs)
            withdrawn=0
            if printf '%s\\n' \"\$exports\" | awk '\$1==\"/mnt/efs\" {found=1} END {exit !found}'; then
                sudo exportfs -u '*:/mnt/efs'
                withdrawn=1
            fi
            if $remote_stop; then exit 0; else
                rc=\$?
                if [ \"\$withdrawn\" = 1 ]; then
                    sudo exportfs -r || echo 'ERROR: NFS export restore failed' >&2
                fi
                exit \$rc
            fi"
    fi
    ssh_node_once "$1" "$remote_stop"
}
