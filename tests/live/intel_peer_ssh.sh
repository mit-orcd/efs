#!/bin/sh
case "$1" in
 intel1_efs) exec timeout "${EFS_SSH_TIMEOUT:-15}s" sh -c "$2" ;;
 intel2_efs) exec timeout "${EFS_SSH_TIMEOUT:-15}s" ssh intel2_efs "$2" ;;
 *) exit 2 ;;
esac
