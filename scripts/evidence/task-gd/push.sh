#!/bin/bash
# push.sh NAME: stream $GD_OUT/NAME and its ops files to ~erbmi1/efs-artifacts/taskGD-20261111/NAME/ and write SHA256SUMS there.
set -u
H=$(cd "$(dirname "$0")" && pwd); OUT=${GD_OUT:-/tmp/gd/out}; name=$1
cd $OUT && COPYFILE_DISABLE=1 tar -s ",^ops/,$name/ops/," -cf - $name ops/$name.* 2>/dev/null | $H/task_gd_ssh.sh fcstor012 \
  "A=\$HOME/efs-artifacts/taskGD-20261111; mkdir -p \$A && tar --warning=no-unknown-keyword -C \$A -xf - && cd \$A/$name && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS && sha256sum SHA256SUMS"
