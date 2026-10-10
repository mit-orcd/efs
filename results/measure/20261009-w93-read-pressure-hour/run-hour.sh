#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-read-pressure-hour-20261009
cd "$base"
bash diagnostic.sh > diagnostic-controller.log 2>&1
cd /data1/efs/w93-read-build
make -j4 EFS_GIT_ID=ea23775b-read-pressure EFS_GIT_DESCRIBE=ea23775b EFS_GIT_BRANCH=devel-persistence-pressure efsd efs-fuse efs-mgmt efs-query efs-bench tests/test_raft tests/test_raft_store tests/test_wire > "$base/build.log" 2>&1
{ ./tests/test_raft; ./tests/test_raft_store; ./tests/test_wire; python3 tests/test_host_retire.py; python3 tests/test_host_checkpoint.py; python3 tests/test_host_flow.py; python3 tests/test_recv_overlap.py; } > "$base/core-verdicts.log" 2>&1
cd "$base"
bash run-pressure.sh > pressure-controller.log 2>&1
