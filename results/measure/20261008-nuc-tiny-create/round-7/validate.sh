#!/bin/bash
set -euo pipefail
pid=$(pgrep -f '^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-profile-r7-fix$')
kill -STOP "$pid"
trap 'kill -CONT "$pid"' EXIT
cd /data1/efs/src
EFS_QUERY_RPC=1 EFS_QUERY_FUSE=1 EFS_QUERY_POSIX=1 python3 tests/live/query_publication.py /data1/efs/tiny-r7-validation-20261008 --mode buffered
