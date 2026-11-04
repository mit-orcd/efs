#!/bin/bash
# Task K real-host test dispatcher. No BatchMode; use the authenticated master.
set -euo pipefail
host=${1:?host}; shift
remote="$*"
case "$host" in
 node9901) ;;
 node9900)
 remote=${remote//\/data1\/efs\/mnt//data1/efs/orcd-mini-mnt}
 remote=${remote//\/data1\/efs\/src\///data1/efs/orcd-mini-client/src/}
 ;;
 *) echo 'Task K dispatcher permits only node9900 and node9901' >&2; exit 2;;
esac
master=${ORCD_SSH_MASTER:-/private/tmp/efs-taskI-orcd-20261026.sock}
exec ssh -o ConnectTimeout=20 -o "ProxyCommand=ssh -S $master -W %h:%p erbmi1@orcd-login.mit.edu" "erbmi1@$host" "$remote"
