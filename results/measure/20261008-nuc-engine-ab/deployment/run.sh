#!/bin/bash
set -u
cd /data1/efs/bench/nuc-engine-ab-20261008/src
make test > ../test.log 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then
  sha256sum efsd efs-fuse efs-mgmt > ../binary-sha256.txt
  python3 scripts/ab_storage_engines_nuc.py --base /data1/efs/bench/nuc-engine-ab-20261008 --seconds 30 --repeat 2 > ../progress.log 2>&1
  rc=$?
fi
echo "$rc" > ../exit-code
