#!/bin/bash
# orcd-mini Task K fourth-member lifecycle. Existing devops config owns n1..n3.
# On node9901, start only AFTER the existing three-member lifecycle is running.
set -euo pipefail
[[ $(hostname -s) == node9901 && $(id -un) == erbmi1 ]] || exit 2
base=/data1/efs
case ${1:?start or stop} in
 start)
 test -d "$base/n1" && test -d "$base/n2" && test -d "$base/n3"
 [[ $("$base/src/efsd" --version) == *00e85a299531* ]] || exit 2
 if [[ -f "$base/logs/efsd-n4.pid" ]]; then
   echo 'Existing n4 record: inspect ownership; not starting a second daemon' >&2; exit 2
 fi
 mkdir -p "$base/n4"
 EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 nohup "$base/src/efsd" \
   --join 10.1.99.1:17432 --listen 10.1.99.1:17435 "$base/n4" --quota 720G \
   >> "$base/logs/efsd-n4.log" 2>&1 < /dev/null &
 echo $! > "$base/logs/efsd-n4.pid"
 python3 "$base/src/scripts/server_processes.py" record "$base/logs/efsd-n4.pid" "$base/n4"
 ;;
 stop)
 # Stop BOTH configured clients cleanly before stopping any daemon.
 python3 "$base/src/scripts/server_processes.py" stop-file "$base/logs/efsd-n4.pid" "$base/n4"
 ;;
 *) exit 2;;
esac
