#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/w73-w64-gates-v2.lock
flock 9
out=/data1/efs/w73-w64-handoff-fix-de2f8b286694
[ "$(cat "$out/runner-exit")" = 0 ]
release=/data1/efs/gate-closure-release-393f5bae
mkdir "$release"
tar -xf /data1/efs/efs-gate-closure-393f5bae.tar -C "$release"
cd "$release"
python3 - <<'PY'
from pathlib import Path
import hashlib
r=Path('.')
d=hashlib.sha256()
paths=[r/'Makefile']+[p for f in ('src','include') for p in (r/f).rglob('*') if p.suffix in ('.c','.h')]
for p in sorted(paths):d.update(str(p).encode()+b'\0'+p.read_bytes())
assert d.hexdigest()=='7d080221cba7aec4163a3a2a1732d5a69e160dd7d10f8499e110dc6766150fc2',d.hexdigest()
print('release source matches accepted candidate',d.hexdigest())
PY
make -j8 efsd efs-fuse efs-mgmt efs-bench efs-query EFS_GIT_ID=393f5bae2c00 EFS_GIT_DESCRIBE=393f5bae2c00 EFS_GIT_BRANCH=devel-gate-closure
sha256sum efsd efs-fuse efs-mgmt efs-bench efs-query > release-binary-sha256.txt
./efsd --version
