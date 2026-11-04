#!/bin/bash
set -euo pipefail
art=/data1/efs/evidence/taskK-orcd-mini-20261027
exec > "$art/build-controller.log" 2>&1
trap 'echo $? > "$art/build-exit"' EXIT
cd /tmp/efs-taskK-mini-20261027/source
export PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig
make -j8 EFS_GIT_ID=00e85a299531 EFS_GIT_DESCRIBE=00e85a299531 EFS_GIT_BRANCH=agent/devel-task-k FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,/data1/efs/deps/libfuse/lib64" efsd efs-fuse efs-mgmt efs-bench
./efsd --version
sha256sum efsd efs-fuse efs-mgmt efs-bench > "$art/binaries.sha256"
{ hostname; uname -a; ip -o -4 addr; ibv_devinfo -d mlx5_0; df -h /data1; } > "$art/environment.txt" 2>&1
