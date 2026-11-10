#!/bin/bash
# fi_build.sh COMMIT... — Task FI: on the Linux host, build efsd, efs-fuse and
# efs-mgmt of each COMMIT from a `git archive` tree that must already be in
# /tmp/efs-taskFI/<COMMIT>, against the private libfuse (EFS_FUSE_PREFIX
# defaults to /data1/efs/deps/libfuse), with EFS_GIT_ID set to the commit so
# that every build gets its own id and no `.git` is needed. Builds are
# default release flags (-O3 -g -fno-omit-frame-pointer), the same as a
# deployed release.
set -u
export TMPDIR=${TMPDIR:-/tmp/efs-taskFI-tmp}
P=${EFS_FUSE_PREFIX:-/data1/efs/deps/libfuse}
export EFS_FUSE_PREFIX=$P PKG_CONFIG_PATH=$P/lib64/pkgconfig LD_LIBRARY_PATH=$P/lib64
for c in "$@"; do
  cd "/tmp/efs-taskFI/$c" || exit 2
  make -j"${FI_JOBS:-16}" EFS_GIT_ID="$c" FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$P/lib64" \
    efsd efs-fuse efs-mgmt > "$TMPDIR/build-$c.log" 2>&1
  echo "$c rc=$? $(ls efsd efs-fuse efs-mgmt 2>&1 | tr '\n' ' ')"
done
