#!/bin/bash
# orcd_mini_ssh.sh HOST REMOTE — run REMOTE on an orcd-mini host through the
# ORCD login node. node9901 runs it as given; for node9900 the cluster's mount
# and release paths become that client's own (/data1/efs/orcd-mini-mnt and
# /data1/efs/orcd-mini-client/src/), so one command line serves both sides.
set -u
h=$1; shift; r="$*"
case $h in
  node9901) ;;
  node9900) r=${r//\/data1\/efs\/mnt//data1/efs/orcd-mini-mnt}
            r=${r//\/data1\/efs\/src\///data1/efs/orcd-mini-client/src/} ;;
  *) echo "unknown orcd-mini host $h" >&2; exit 2 ;;
esac
exec ssh -o BatchMode=yes -o ConnectTimeout=20 -o ProxyCommand="ssh -W %h:%p ${ORCD_LOGIN:-orcd-login}" \
    "${ORCD_USER:-erbmi1}@$h" "$r"
